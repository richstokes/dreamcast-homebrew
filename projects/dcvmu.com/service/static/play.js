// Boots the release asset named on the page in EmulatorJS's Flycast core.
(function () {
  var game = document.getElementById('dreamcast-game');
  if (!game) return;
  var status = document.getElementById('player-status');
  var DATA = 'https://cdn.emulatorjs.org/4.2.3/data/';
  // The HLE BIOS lets homebrew ELFs direct-boot without Sega firmware.
  var OPTIONS = {
    reicast_boot_to_bios: 'disabled',
    reicast_hle_bios: 'enabled',
    reicast_threaded_rendering: 'disabled',
    reicast_synchronous_rendering: 'disabled',
    reicast_internal_resolution: '640x480',
    reicast_enable_dsp: 'disabled',
    reicast_mipmapping: 'disabled',
    reicast_anisotropic_filtering: 'off',
    reicast_enable_rttb: 'disabled',
    reicast_enable_purupuru: 'disabled',
    reicast_alpha_sorting: 'per-strip (fast, least accurate)',
    reicast_delay_frame_swapping: 'disabled',
    reicast_frame_skipping: 'disabled',
    reicast_framerate: 'normal'
  };
  var IOS_OPTIONS = Object.assign({}, OPTIONS, {
    reicast_internal_resolution: '320x240',
    reicast_frame_skipping: '1'
  });
  var params = new URLSearchParams(window.location.search);
  var state = '';
  var online = false;

  function setStatus(text) {
    state = text;
    status.textContent = online ? text + ' \u00b7 online through DCNet' : text;
  }

  function setOnline(value) {
    online = value;
    setStatus(state);
  }

  // The core's emulated broadband adapter sends and receives Ethernet frames
  // through this object (tools/flycast-wasm/patches), one per WebSocket
  // message to dcvmu.com's relay to DCNet (netrelay.py). Its presence turns
  // the adapter on; the socket opens when a game first uses the network.
  var net = { started: false, socket: null, retry: null, inbox: [], outbox: [] };

  function connectNet() {
    var scheme = window.location.protocol === 'https:' ? 'wss://' : 'ws://';
    var socket = net.socket = new WebSocket(scheme + window.location.host + '/play/net');
    socket.binaryType = 'arraybuffer';
    socket.onopen = function () {
      // Sending what queued while connecting saves KOS a slow DHCP retry.
      net.outbox.forEach(function (frame) { socket.send(frame); });
      net.outbox = [];
      setOnline(true);
    };
    socket.onmessage = function (event) {
      if (net.inbox.length < 256) net.inbox.push(new Uint8Array(event.data));
    };
    socket.onclose = function () {
      if (net.socket !== socket) return;
      net.socket = null;
      setOnline(false);
      if (net.started) net.retry = setTimeout(connectNet, 5000);
    };
  }

  window.flycastNet = {
    start: function () {
      net.started = true;
      if (!net.socket) connectNet();
    },
    stop: function () {
      net.started = false;
      clearTimeout(net.retry);
      var socket = net.socket;
      net.socket = null;
      if (socket) socket.close();
      net.inbox = [];
      net.outbox = [];
      setOnline(false);
    },
    send: function (frame) {
      if (net.socket && net.socket.readyState === WebSocket.OPEN) net.socket.send(frame);
      else if (net.outbox.length < 64) net.outbox.push(frame);
    },
    recv: function () {
      return net.inbox.shift();
    }
  };

  function supportsWebGL2() {
    try {
      return Boolean(document.createElement('canvas').getContext('webgl2'));
    } catch (e) {
      return false;
    }
  }

  function isIOS() {
    return /iP(?:hone|ad|od)/.test(navigator.userAgent) ||
      (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1) ||
      params.has('ios-compat');
  }

  // Desktop OpenGL reads GL_EXTENSIONS through glGetString. WebGL rejects
  // that enum, so answer it with the WebGL extension list instead.
  function patchWebGL() {
    var getContext = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function () {
      var context = getContext.apply(this, arguments);
      var webgl = (typeof WebGLRenderingContext !== 'undefined' && context instanceof WebGLRenderingContext) ||
        (typeof WebGL2RenderingContext !== 'undefined' && context instanceof WebGL2RenderingContext);
      if (webgl && !context.flycastPatched) {
        var getParameter = context.getParameter.bind(context);
        context.getParameter = function (parameter) {
          if (parameter === 0x1f03) return (context.getSupportedExtensions() || []).join(' ');
          return getParameter(parameter);
        };
        context.flycastPatched = true;
      }
      return context;
    };
  }

  // Flycast emits many small modules while its SH4 JIT warms up. Letting
  // WebKit compile them all at once can exceed iOS's process limit.
  function queueSmallWasmCompiles() {
    var instantiate = WebAssembly.instantiate;
    var queue = Promise.resolve();
    WebAssembly.instantiate = function (source, imports) {
      var size = source instanceof ArrayBuffer || ArrayBuffer.isView(source) ? source.byteLength : 0;
      var compile = function () { return instantiate.call(WebAssembly, source, imports); };
      if (!size || size > 1024 * 1024) return compile();
      var queued = queue.then(compile, compile);
      queue = queued.then(function () {}, function () {});
      return queued;
    };
  }

  // EmulatorJS keys save states by a numeric game ID; FNV-1a of tag/asset.
  function gameId(value) {
    var hash = 2166136261;
    for (var i = 0; i < value.length; i += 1) {
      hash ^= value.charCodeAt(i);
      hash = Math.imul(hash, 16777619);
    }
    return hash >>> 0;
  }

  if (!supportsWebGL2()) {
    setStatus('This player needs a browser and graphics driver with WebGL 2 support.');
    return;
  }
  patchWebGL();
  var ios = isIOS();
  if (ios) queueSmallWasmCompiles();

  var name = game.getAttribute('data-name');
  var core = game.getAttribute('data-core');
  // The core's ZIP reader unpacks Flycast's embedded resources with fflate.
  window.__dreamcastUnzipSync = window.fflate.unzipSync;
  window.EJS_player = '#dreamcast-game';
  window.EJS_core = 'flycast';
  window.EJS_gameUrl = game.getAttribute('data-rom');
  window.EJS_gameName = name;
  window.EJS_gameID = gameId(game.getAttribute('data-game'));
  window.EJS_pathtodata = DATA;
  window.EJS_paths = {
    'flycast.json': game.getAttribute('data-report'),
    'flycast-wasm.data': core,
    'flycast-legacy-wasm.data': core
  };
  window.EJS_defaultOptions = ios ? IOS_OPTIONS : OPTIONS;
  window.EJS_backgroundColor = '#03050a';
  window.EJS_color = '#e95620';
  window.EJS_volume = 0.7;
  window.EJS_threads = false;
  window.EJS_disableCue = true;
  window.EJS_DEBUG_XX = params.has('debug');
  window.EJS_startOnLoaded = false;
  window.EJS_startButtonName = 'Launch ' + name;
  window.EJS_language = 'en-US';
  window.EJS_disableAutoLang = false;
  // Continuous release assets are replaced in place, so keep no ROM bytes in IndexedDB.
  window.EJS_CacheLimit = 0;
  window.EJS_ready = function () { setStatus('Ready'); };
  window.EJS_onGameStart = function () { setStatus('Playing'); };
  window.EJS_onExit = function () { setStatus('Stopped'); };

  var loader = document.createElement('script');
  loader.src = DATA + 'loader.js';
  loader.crossOrigin = 'anonymous';
  loader.async = true;
  loader.onerror = function () { setStatus('The emulator could not be loaded. Please try again.'); };
  document.body.appendChild(loader);
}());
