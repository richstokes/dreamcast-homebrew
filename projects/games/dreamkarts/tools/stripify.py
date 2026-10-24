"""Triangle strips for the PowerVR, shared by the Blender mesh exporters
(tools/build_canyon_rock.py, tools/build_tsudoken_props.py)."""

from __future__ import annotations

import heapq


def stripify(tris, swaps=True):
    """Strips for the PowerVR, where every strip costs a submit of its own,
    so fewer and longer is what matters. Each strip starts from the unused
    triangle with the fewest unused neighbours (it would otherwise be
    stranded as a strip of one), takes the best of its three rotations,
    and grows from both ends. The game draws with culling off, so a strip
    may run on through any neighbour across its last edge, whatever the
    winding."""
    edge_tris = {}
    for t, (a, b_, c) in enumerate(tris):
        for e in ((a, b_), (b_, c), (c, a)):
            edge_tris.setdefault(frozenset(e), []).append(t)
    used = [False] * len(tris)

    def degree(t):
        a, b_, c = tris[t]
        return sum(1 for e in ((a, b_), (b_, c), (c, a)) for u in edge_tris[frozenset(e)] if u != t and not used[u])

    def grow(seq, taken):
        while True:
            e = frozenset(seq[-2:])
            nxt = next((t for t in edge_tris.get(e, ()) if not used[t] and t not in taken), None)
            if nxt is not None:
                taken.add(nxt)
                seq.append(next(v for v in tris[nxt] if v not in e))
                continue
            if not swaps or len(seq) < 3:
                return seq
            # A swap: across the last triangle's other free edge (the
            # newest vertex and the one before last), reached by repeating
            # the one before last, a degenerate triangle the hardware drops.
            e2 = frozenset((seq[-3], seq[-1]))
            nxt = next((t for t in edge_tris.get(e2, ()) if not used[t] and t not in taken), None)
            if nxt is None:
                return seq
            taken.add(nxt)
            seq.append(seq[-3])
            seq.append(next(v for v in tris[nxt] if v not in e2))

    heap = [(degree(t), t) for t in range(len(tris))]
    heapq.heapify(heap)
    strips = []
    while heap:
        d, t = heapq.heappop(heap)
        if used[t]:
            continue
        nd = degree(t)
        if nd != d:
            heapq.heappush(heap, (nd, t))
            continue
        a, b_, c = tris[t]
        best = None
        for rot in ((a, b_, c), (b_, c, a), (c, a, b_)):
            taken = {t}
            seq = grow(list(rot), taken)
            seq = grow(seq[::-1], taken)          # and back the other way
            if best is None or len(seq) > len(best[0]):
                best = (seq, taken)
        for u in best[1]:
            used[u] = True
        strips.append((best[0], sorted(best[1])))
    return strips


