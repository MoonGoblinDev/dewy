#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(void){ size_t n=8u<<20; char*a=malloc(n),*b=malloc(n); memset(a,1,n); memset(b,2,n);
 struct timespec t0,t1; double best=0;
 for(int r=0;r<5;r++){ clock_gettime(CLOCK_MONOTONIC,&t0); for(int i=0;i<60;i++){ memcpy(b,a,n); a[i]=b[i+1]; }
  clock_gettime(CLOCK_MONOTONIC,&t1); double s=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9; double gb=60.0*n/s/1e9; if(gb>best)best=gb; }
 printf("memcpy %.1f GB/s (8 MB, like a camera frame)\n",best); return 0;}
