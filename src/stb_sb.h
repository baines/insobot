// stb stretchy_buffer.h v1.02 nothings.org/stb
// with custom addtions sb_end, sb_pop, sb_erase

#ifndef STB_STRETCHY_BUFFER_H_INCLUDED
#define STB_STRETCHY_BUFFER_H_INCLUDED

#ifndef NO_STRETCHY_BUFFER_SHORT_NAMES
#define sb_free   stb_sb_free
#define sb_push   stb_sb_push
#define sb_count  stb_sb_count
#define sb_add    stb_sb_add
#define sb_last   stb_sb_last
#define sb_end    stb_sb_end
#define sb_pop    stb_sb_pop
#define sb_erase  stb_sb_erase
#define sb_each   stb_sb_each
#endif

#define sb(type) type*

#define stb_sb_free(a)         ((a) ? free(stb__sbraw(a)),(a)=0,0 : 0)
#define stb_sb_push(a,v)       (stb__sbmaybegrow(a,1), (a)[stb__sbn(a)++] = (v))
#define stb_sb_count(a)        ((a) ? stb__sbn(a) : 0)
#define stb_sb_add(a,n)        (stb__sbmaybegrow(a,n), stb__sbn(a)+=(n), &(a)[stb__sbn(a)-(n)])
#define stb_sb_last(a)         ((a)[stb__sbn(a)-1])
#define stb_sb_end(a)          ((a) ? (a) + stb__sbn(a) : 0)
#define stb_sb_pop(a)          (--stb__sbn(a))
#define stb_sb_erase(a,i)      ((a) ? memmove((a)+(i), (a)+(i)+1, sizeof(*(a))*((--stb__sbn(a))-(i))),0 : 0);

#define stb_sb_each(n,h)       for(typeof(h) n = h; n < sb_end(h); ++n)

#define stb__sbraw(a) ((size_t *) (a) - 2)
#define stb__sbm(a)   stb__sbraw(a)[0]
#define stb__sbn(a)   stb__sbraw(a)[1]

#define stb__sbneedgrow(a,n)  ((a)==0 || stb__sbn(a)+(n) >= stb__sbm(a))
#define stb__sbmaybegrow(a,n) (stb__sbneedgrow(a,(n)) ? stb__sbgrow(a,n) : 0)
#define stb__sbgrow(a,n)      ((a) = stb__sbgrowf((a), (n), sizeof(*(a))))

#include <stdlib.h>

static inline void * stb__sbgrowf(void *arr, int increment, int itemsize)
{
   size_t inc_cur = arr ? stb__sbm(arr) + (stb__sbm(arr) >> 1) : 0;
   size_t min_needed = stb_sb_count(arr) + increment;
   size_t m = inc_cur > min_needed ? inc_cur : min_needed;
   size_t *p = (size_t *) realloc(arr ? stb__sbraw(arr) : 0, itemsize * m + sizeof(size_t)*2);
   if (p) {
      if (!arr)
         p[1] = 0;
      p[0] = m;
      return p+2;
   } else {
      #ifdef STRETCHY_BUFFER_OUT_OF_MEMORY
      STRETCHY_BUFFER_OUT_OF_MEMORY ;
      #endif
      return (void *) (2*sizeof(size_t)); // try to force a NULL pointer exception later
   }
}

#ifdef STB_SB_MMAP

#include <sys/mman.h>
#include <stdio.h>

#define sbmm_free(a)    ((a) ? munmap(stb__sbraw(a), stb__sbm(a)),(a)=0,0 : 0)
#define sbmm_push(a,v)  (stb__sbmaybegrow_mm(a,1), (a)[stb__sbn(a)++] = (v))
#define sbmm_add(a,n)   (stb__sbmaybegrow_mm(a,n), stb__sbn(a)+=(n), &(a)[stb__sbn(a)-(n)])

#define sbmm_count   stb_sb_count
#define sbmm_last    stb_sb_last
#define sbmm_end     stb_sb_end
#define sbmm_pop     stb_sb_pop
#define sbmm_erase   stb_sb_erase

#define stb__sbmaybegrow_mm(a,n) (stb__sbneedgrow(a,(n)) ? stb__sbgrow_mm(a,n) : 0)
#define stb__sbgrow_mm(a,n)      ((a) = stb__sbgrowf_mm((a), (n), sizeof(*(a))))

#define SB_PAGE_SIZE 4096

static inline void * stb__sbgrowf_mm(void *arr, int increment, int itemsize)
{
   size_t inc_cur = arr ? stb__sbm(arr) + SB_PAGE_SIZE : 0;
   size_t min_needed = stb_sb_count(arr) + increment;
   size_t m = inc_cur > min_needed ? inc_cur : min_needed;

   size_t mem_needed = m * itemsize + sizeof(size_t) * 2;
   mem_needed = (mem_needed + (SB_PAGE_SIZE-1)) & ~(SB_PAGE_SIZE-1);

   size_t mem_have = !arr ? 0 : stb__sbm(arr) * itemsize + sizeof(size_t) * 2;
   mem_have = (mem_have + (SB_PAGE_SIZE-1)) & ~(SB_PAGE_SIZE-1);

   size_t* p = 0;
   if(arr){
	   p = mremap(
		   stb__sbraw(arr),
		   mem_have,
		   mem_needed,
		   MREMAP_MAYMOVE
	   );
	   if(p == MAP_FAILED){
		   perror("mremap");
	   }
   } else {
	   p = mmap(
		   0,
		   mem_needed,
		   PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS,
		   -1,
		   0
	   );
	   if(p == MAP_FAILED){
		   perror("mmap");
	   }
   }

   if (p != MAP_FAILED) {
      if (!arr)
         p[1] = 0;
      p[0] = m;
      return p+2;
   } else {
      #ifdef STRETCHY_BUFFER_OUT_OF_MEMORY
      STRETCHY_BUFFER_OUT_OF_MEMORY ;
      #endif
      return (void *) (2*sizeof(size_t)); // try to force a NULL pointer exception later
   }
}

#endif // STB_SB_MMAP

#endif // STB_STRETCHY_BUFFER_H_INCLUDED
