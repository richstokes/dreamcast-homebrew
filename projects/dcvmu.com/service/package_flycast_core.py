#!/usr/bin/env python3
"""Package the Flycast core the web player loads into static/play.

tools/flycast-wasm/build.sh builds the GPLv2 core (Flycast with the
WebAssembly JIT from https://github.com/nasomers/flycast-wasm v1.0 and the
patches in tools/flycast-wasm/patches). This turns its output into
flycast-wasm.data for EmulatorJS 4.2.3, a ZIP that EmulatorJS detects by its
signature despite the .data extension, and writes flycast.json to match.
Changes on top of the build:

- core.js: real ZIP and ELF functions replace abort stubs, so homebrew ELFs
  direct-boot with the HLE BIOS, and evicted JIT blocks free their function
  table slot, which keeps iOS under its JIT limit
- core.wasm: the heap starts at 64 MiB instead of 256 MiB and still grows,
  so iOS is less likely to kill the tab

Usage: package_flycast_core.py [BUILD_OUT], default
$FLYCAST_WASM_WORK/out (~/.local/share/dreamcast/flycast-wasm/out).
"""
import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

WORK = Path(os.environ.get('FLYCAST_WASM_WORK', Path.home() / '.local/share/dreamcast/flycast-wasm'))
PLAYER = Path(__file__).resolve().parent / 'static' / 'play'
HEAP_PAGES = 1024  # 64 KiB WebAssembly pages: 64 MiB.

# Flycast's embedded-resource ZIP reader (via fflate) and its 32-bit ELF parser.
# Upstream left these as abort stubs, so homebrew ELFs could not boot.
ZIP_HELPERS = '\n'.join((
    'var __dreamcastZipHandles=new Map(),__dreamcastZipNext=1048576;',
    'function __dreamcastZipHandle(value){var handle=__dreamcastZipNext++;__dreamcastZipHandles.set(handle,value);return handle}',
    'function __dreamcastZipIndex(value){return typeof value==="bigint"?Number(value):value}',
    'function __dreamcastZipWriteU64(pointer,value){HEAPU32[pointer>>2]=value>>>0;HEAPU32[(pointer+4)>>2]=Math.floor(value/4294967296)>>>0}',
    'function __dreamcastZipEntry(archive,index){var item=__dreamcastZipHandles.get(archive);return item&&item.entries[__dreamcastZipIndex(index)]}',
    'function __dreamcastZipStat(entry,index,stat){if(!entry)return-1;entry.namePointer=entry.namePointer||stringToNewUTF8(entry.name);__dreamcastZipWriteU64(stat,7);HEAPU32[(stat+8)>>2]=entry.namePointer;__dreamcastZipWriteU64(stat+16,__dreamcastZipIndex(index));__dreamcastZipWriteU64(stat+24,entry.data.length);return 0}',
    'function _zip_close(archive){__dreamcastZipHandles.delete(archive);return 0}',
))
SHIMS = {
    'zip_close': ZIP_HELPERS,
    'zip_fclose': 'function _zip_fclose(file){__dreamcastZipHandles.delete(file);return 0}',
    'zip_fopen': 'function _zip_fopen(archive,name){var item=__dreamcastZipHandles.get(archive),wanted=UTF8ToString(name);if(!item)return 0;var entry=item.entries.find(function(value){return value.name===wanted});return entry?__dreamcastZipHandle({entry:entry,offset:0}):0}',
    'zip_fopen_index': 'function _zip_fopen_index(archive,index){var entry=__dreamcastZipEntry(archive,index);return entry?__dreamcastZipHandle({entry:entry,offset:0}):0}',
    'zip_fread': 'function _zip_fread(file,buffer,length){var item=__dreamcastZipHandles.get(file);if(!item)return-1n;var count=Math.min(Number(length),item.entry.data.length-item.offset);HEAPU8.set(item.entry.data.subarray(item.offset,item.offset+count),buffer);item.offset+=count;return BigInt(count)}',
    'zip_get_num_entries': 'function _zip_get_num_entries(archive){var item=__dreamcastZipHandles.get(archive);return BigInt(item?item.entries.length:-1)}',
    'zip_open_from_source': 'function _zip_open_from_source(source){var item=__dreamcastZipHandles.get(source);if(!item)return 0;try{var files=window.__dreamcastUnzipSync(item.data),entries=Object.keys(files).map(function(name){return{name:name,data:files[name]}});__dreamcastZipHandles.delete(source);return __dreamcastZipHandle({entries:entries})}catch(error){console.warn("Unable to unpack Flycast resource",error);return 0}}',
    'zip_source_buffer_create': 'function _zip_source_buffer_create(data,length){return __dreamcastZipHandle({data:HEAPU8.slice(data,data+Number(length))})}',
    'zip_source_filep_create': 'function _zip_source_filep_create(){return 0}',
    'zip_source_free': 'function _zip_source_free(source){__dreamcastZipHandles.delete(source)}',
    'zip_stat': 'function _zip_stat(archive,name,flags,stat){var item=__dreamcastZipHandles.get(archive),wanted=UTF8ToString(name);if(!item)return-1;var index=item.entries.findIndex(function(value){return value.name===wanted});return __dreamcastZipStat(item.entries[index],index,stat)}',
    'zip_stat_index': 'function _zip_stat_index(archive,index,flags,stat){return __dreamcastZipStat(__dreamcastZipEntry(archive,index),index,stat)}',
    'elf_newFile': 'function _elf_newFile(file,size,result){size=Number(size);if(size<52||HEAPU8[file]!==127||HEAPU8[file+1]!==69||HEAPU8[file+2]!==76||HEAPU8[file+3]!==70||HEAPU8[file+4]!==1||HEAPU8[file+5]!==1)return-1;var phoff=HEAPU32[(file+28)>>2],shoff=HEAPU32[(file+32)>>2],phentsize=HEAPU16[(file+42)>>1],phnum=HEAPU16[(file+44)>>1],shentsize=HEAPU16[(file+46)>>1],shnum=HEAPU16[(file+48)>>1],shstrndx=HEAPU16[(file+50)>>1];if(phentsize!==32||shentsize!==40||shstrndx>=shnum||phoff+phentsize*phnum>size||shoff+shentsize*shnum>size)return-1;if(result){HEAPU32[result>>2]=file;HEAPU32[(result+4)>>2]=size;HEAPU8[result+8]=1}return 0}',
    'elf_getNumProgramHeaders': 'function _elf_getNumProgramHeaders(elf){var file=HEAPU32[elf>>2];return HEAPU16[(file+44)>>1]}',
    'elf_getProgramHeaderFileSize': 'function _elf_getProgramHeaderFileSize(elf,index){var file=HEAPU32[elf>>2],header=file+HEAPU32[(file+28)>>2]+index*HEAPU16[(file+42)>>1];return HEAPU32[(header+16)>>2]}',
    'elf_getProgramHeaderMemorySize': 'function _elf_getProgramHeaderMemorySize(elf,index){var file=HEAPU32[elf>>2],header=file+HEAPU32[(file+28)>>2]+index*HEAPU16[(file+42)>>1];return HEAPU32[(header+20)>>2]}',
    'elf_getProgramHeaderOffset': 'function _elf_getProgramHeaderOffset(elf,index){var file=HEAPU32[elf>>2],header=file+HEAPU32[(file+28)>>2]+index*HEAPU16[(file+42)>>1];return HEAPU32[(header+4)>>2]}',
    'elf_getProgramHeaderType': 'function _elf_getProgramHeaderType(elf,index){var file=HEAPU32[elf>>2],header=file+HEAPU32[(file+28)>>2]+index*HEAPU16[(file+42)>>1];return HEAPU32[header>>2]}',
    'elf_getProgramHeaderVaddr': 'function _elf_getProgramHeaderVaddr(elf,index){var file=HEAPU32[elf>>2],header=file+HEAPU32[(file+28)>>2]+index*HEAPU16[(file+42)>>1];return HEAPU32[(header+8)>>2]}',
}

JIT_RELEASE = {
    'block': ('function wasm_remove_block(block_pc){',
              'var idx=Module._wasmBlockIdx&&Module._wasmBlockIdx[block_pc];'),
    'chain': ('function wasm_remove_chain(head_pc){',
              'var idx=Module._wasmChainIdx&&Module._wasmChainIdx[head_pc];'),
}
RELEASE_SLOT = 'if(idx){try{wasmTable.set(idx,null);Module._jitReleased=(Module._jitReleased|0)+1}catch(e){}}'


def replace_once(source, old, new):
    if source.count(old) != 1:
        sys.exit(f'Expected one match in the built core.js for: {old[:80]}')
    return source.replace(old, new)


def patch_js(source):
    for name, implementation in SHIMS.items():
        source = replace_once(source, f'function _{name}(){{abort("missing function: {name}")}}', implementation)
    for start, lookup in JIT_RELEASE.values():
        source = replace_once(source, start, start + lookup + RELEASE_SLOT)
    return source


def leb128(data, offset):
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if byte < 0x80:
            return value, offset


def patch_heap(wasm):
    # Rewrite the memory section's initial page count in place, keeping its
    # two-byte LEB128 width so no other offset in the module moves.
    offset = 8
    while offset < len(wasm):
        size, start = leb128(wasm, offset + 1)
        if wasm[offset] == 5:
            count, cursor = leb128(wasm, start)
            flags = wasm[cursor]
            initial, end = leb128(wasm, cursor + 1)
            if count != 1 or flags != 1 or end - cursor - 1 != 2 or initial < HEAP_PAGES:
                sys.exit('Unexpected Flycast memory section')
            patched = bytearray(wasm)
            patched[cursor + 1:end] = bytes((0x80 | (HEAP_PAGES & 0x7f), HEAP_PAGES >> 7))
            return bytes(patched)
        offset = start + size
    sys.exit('Flycast memory section not found')


def main():
    build = Path(sys.argv[1]) if len(sys.argv) > 1 else WORK / 'out'
    members = {
        'core.js': patch_js((build / 'flycast_libretro.js').read_text()).encode(),
        'core.wasm': patch_heap((build / 'flycast_libretro.wasm').read_bytes()),
        'core.json': json.dumps({
            'name': 'flycast',
            'extensions': ['cdi', 'gdi', 'chd', 'cue', 'iso', 'elf', 'bin', 'lst', 'zip', '7z', 'dat'],
            'options': {}, 'save': 'state', 'license': 'GPLv2',
            'repo': 'https://github.com/flyinghead/flycast',
        }, separators=(',', ':')).encode(),
        'build.json': json.dumps({
            'buildDate': '2026-10-26', 'version': '1.0.0-homebrew.4-bba', 'minimumEJSVersion': '4.2.3',
        }, separators=(',', ':')).encode(),
        'license.txt': (WORK / 'flycast-wasm' / 'LICENSE').read_bytes(),
    }
    # Fixed timestamps keep rebuilds byte-identical.
    core = PLAYER / 'flycast-wasm.data'
    with zipfile.ZipFile(core, 'w') as archive:
        for name, data in members.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 8, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    # EmulatorJS keeps the core in IndexedDB until this report's buildStart
    # changes, so tie it to the core's contents.
    version = hashlib.sha256(core.read_bytes()).hexdigest()[:16]
    report = {'buildStart': f'flycast-wasm-{version}', 'options': {'defaultWebGL2': True}}
    (PLAYER / 'flycast.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Wrote {core} and flycast.json')


if __name__ == '__main__':
    main()
