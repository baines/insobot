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
#define sb        stb_sb
#endif

#define stb_sb(x) x*

#define stb_sb_free(a)         ((a) ? free(stb__sbraw(a)),(a)=0,0 : 0)
#define stb_sb_push(a,v)       (stb__sbmaybegrow(a,1), (a)[stb__sbn(a)++] = (v))
#define stb_sb_count(a)        ((a) ? stb__sbn(a) : 0)
#define stb_sb_add(a,n)        (stb__sbmaybegrow(a,n), stb__sbn(a)+=(n), &(a)[stb__sbn(a)-(n)])
#define stb_sb_last(a)         ((a)[stb__sbn(a)-1])
#define stb_sb_end(a)          ((a) ? (a) + stb__sbn(a) : 0)
#define stb_sb_pop(a)          (--stb__sbn(a))
#define stb_sb_erase(a,i)      ((a) ? memmove((a)+(i), (a)+(i)+1, sizeof(*(a))*((--stb__sbn(a))-(i))),0 : 0);

#define stb__sbraw(a) ((size_t *) (a) - 2)
#define stb__sbm(a)   stb__sbraw(a)[0]
#define stb__sbn(a)   stb__sbraw(a)[1]

#define stb__sbneedgrow(a,n)  ((a)==0 || stb__sbn(a)+(n) >= stb__sbm(a))
#define stb__sbmaybegrow(a,n) (stb__sbneedgrow(a,(n)) ? stb__sbgrow(a,n) : 0)
#define stb__sbgrow(a,n)      ((a) = stb__sbgrowf((a), (n), sizeof(*(a))))

#define stb_sb_each(n,h)       for(typeof(h) n = h; n < sb_end(h); ++n)

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

#endif // STB_STRETCHY_BUFFER_H_INCLUDED
