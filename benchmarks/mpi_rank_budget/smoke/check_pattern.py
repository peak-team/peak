"""Independent small direct simulation of ALL 32-rank/6-thread routing."""
A=6364136223846793005;B=1442695040888963407;mask=(1<<64)-1
# Composition independently evaluated as affine coefficient pairs.
def compose(f,g):return ((f[0]*g[0])&mask,(f[0]*g[1]+f[1])&mask)
def power(f,n):
 r=(1,0)
 while n:
  if n&1:r=compose(f,r)
  f=compose(f,f);n//=2
 return r
ranks=32;threads=6;epochs=529;calls=17;profiled=0;coverage=set()
for rank in range(ranks):
 for thread in range(threads):
  x=y=0x12345678+rank*100003+thread
  for e in range(epochs):
   id=(e//ranks)%16;target=id if rank==e%ranks and thread==0 else id+128
   if target<16:profiled+=calls;coverage.add((rank,target))
   for c in range(calls):x=(A*x+B+target%128)&mask
   a,b=power((A,B+id),calls);y=(a*y+b)&mask
  assert x==y,(rank,thread)
assert profiled==epochs*calls
assert len(coverage)==ranks*16
print('PASS direct vs affine oracle across32ranks6threads529epochs, all512 rank/targetpairs, exactglobalprofiledcallcount')
