/* cc -I templates/runtime tests/test_xmm_unary.c -lm -o /tmp/test_xmm_unary */
#include "recomp_types.h"
#include <assert.h>
#include <stdlib.h>
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; abort(); }
static void approximate(float result, float exact) {
 assert(fabsf(result-exact) <= fabsf(exact) * (1.5f/4096.0f));
}
int main(void) {
 RecompXmm a={{0}},b;
 const float values[4]={1,4,16,64};
 for(int i=0;i<4;i++)a.f[i]=values[i];
 b=XMM_SQRT(a);for(int i=0;i<4;i++)assert(b.f[i]==sqrtf(values[i]));
 b=XMM_RSQRT(a);for(int i=0;i<4;i++)approximate(b.f[i],1.0f/sqrtf(values[i]));
 b=XMM_RCP(a);for(int i=0;i<4;i++)approximate(b.f[i],1.0f/values[i]);
 a=XMM_RSQRT(a);for(int i=0;i<4;i++)approximate(a.f[i],1.0f/sqrtf(values[i]));
 a.f[0]=0;a.f[1]=-0.0f;a.f[2]=INFINITY;a.f[3]=NAN;
 b=XMM_RSQRT(a);assert(isinf(b.f[0])&&!signbit(b.f[0]));assert(isinf(b.f[1])&&signbit(b.f[1]));assert(b.f[2]==0);assert(isnan(b.f[3]));
 b=XMM_RCP(a);assert(isinf(b.f[0])&&!signbit(b.f[0]));assert(isinf(b.f[1])&&signbit(b.f[1]));assert(b.f[2]==0);assert(isnan(b.f[3]));
 b=XMM_SQRT(a);assert(b.f[0]==0&&!signbit(b.f[0]));assert(b.f[1]==0&&signbit(b.f[1]));assert(isinf(b.f[2]));assert(isnan(b.f[3]));
 a.f[0]=-1;b=XMM_RSQRT(a);assert(isnan(b.f[0]));b=XMM_SQRT(a);assert(isnan(b.f[0]));
 return 0;
}
