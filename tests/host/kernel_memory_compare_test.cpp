#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include "kernel/memory_compare.hpp"
static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr,"kernel-memory-compare-test: FAIL %s\n",why); std::exit(1); }
}
static int reference(const unsigned char* a, const unsigned char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
int main() {
    unsigned char a[160], b[160];
    for (unsigned left = 0; left < 16; ++left) for (unsigned right = 0; right < 16; ++right) {
        for (unsigned n = 0; n <= 128; ++n) {
            for (unsigned i = 0; i < n; ++i) a[left+i] = b[right+i] = (i*37+131)%256;
            check(!kernel_memory::compare(a+left,b+right,n),"equal bytes at every alignment and word boundary");
            for (unsigned i = 0; i < n; ++i) {
                b[right+i] ^= 0x80;
                check(kernel_memory::compare(a+left,b+right,n) == reference(a+left,b+right,n),"first differing unsigned byte determines order");
                b[right+i] ^= 0x80;
            }
        }
    }
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    auto* guarded = static_cast<unsigned char*>(mmap(nullptr,4*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    check(guarded != MAP_FAILED,"allocate guard pages");
    check(!mprotect(guarded+page,page,PROT_NONE) && !mprotect(guarded+3*page,page,PROT_NONE),"protect bytes after both operands");
    for (unsigned n = 0; n <= 128; ++n) {
        auto* lhs = guarded+page-n; auto* rhs = guarded+3*page-n;
        for (unsigned i = 0; i < n; ++i) lhs[i] = rhs[i] = (unsigned char)i;
        check(!kernel_memory::compare(lhs,rhs,n),"word and tail loads never read past the requested count");
        if (n) { rhs[n-1] ^= 0x80; check(kernel_memory::compare(lhs,rhs,n) == reference(lhs,rhs,n),"mismatching tails respect guard pages"); }
    }
    check(!kernel_memory::compare(nullptr,nullptr,0),"zero-byte comparisons do not dereference pointers");
    (void)munmap(guarded,4*page);
    std::puts("kernel-memory-compare-test: PASS (all alignments, every mismatch, unsigned order and guard pages)");
}
