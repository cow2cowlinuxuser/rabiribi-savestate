"""Compare two gh_trace.txt dumps and characterise where they diverge."""
import sys, collections

def load(p):
    ops, head = [], None
    for line in open(p):
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            if head is None:
                head = line
            continue
        op, size, off = line.split()
        ops.append((op, int(size, 16), int(off, 16)))
    return head, ops

ha, A = load(sys.argv[1])
hb, B = load(sys.argv[2])
print(f"prev {ha}\nnow  {hb}\n{len(A)} vs {len(B)} operation(s)\n")

# The op/size stream is the program's behaviour; the offset is the allocator's.
sa = [(o, s) for o, s, _ in A]
sb = [(o, s) for o, s, _ in B]
print("op+size stream identical:", sa == sb)

diff = [i for i in range(min(len(A), len(B))) if A[i] != B[i]]
print(f"lines whose offset differs: {len(diff)} of {len(A)}")
if diff:
    print(f"first divergence at operation {diff[0]}, last at {diff[-1]}")

# Is the set of addresses handed out the same, just in a different order?
print("\nmultiset of (op,size,offset) identical:", collections.Counter(A) == collections.Counter(B))
print("set of offsets ever used identical:",
      {o for _, _, o in A} == {o for _, _, o in B})

# Which allocation sizes are unstable?
bysize = collections.Counter()
total = collections.Counter()
for i in range(min(len(A), len(B))):
    total[A[i][1]] += 1
    if A[i] != B[i]:
        bysize[A[i][1]] += 1
print("\nsize      unstable/total")
for size, n in bysize.most_common(12):
    print(f"  {size:#010x} {size:>9}   {n}/{total[size]}")
stable = [s for s in total if s not in bysize]
print(f"\n{len(stable)} distinct size(s) never moved, {len(bysize)} moved")
if bysize:
    print(f"largest size that ever moved: {max(bysize):#x} ({max(bysize)} bytes)")

# Does the entity table -- the first allocation -- stay put?
print(f"\nfirst allocation  prev: size {A[0][1]:#x} at +{A[0][2]:#x}"
      f"   now: size {B[0][1]:#x} at +{B[0][2]:#x}")

# Of the blocks still live at the end, how many sit at a stable offset?
def live(ops):
    cur = {}
    for op, s, off in ops:
        if op in 'ACR':
            cur.setdefault(off, (op, s))
        elif op == 'F':
            cur.pop(off, None)
    return cur
la, lb = live(A), live(B)
same = set(la) & set(lb)
print(f"live at the end: {len(la)} prev, {len(lb)} now, {len(same)} at the same offset")
big = [o for o in la if la[o][1] >= 0x1000]
bigsame = [o for o in big if o in lb]
print(f"of the live blocks >= 4 KB: {len(bigsame)}/{len(big)} at the same offset")
