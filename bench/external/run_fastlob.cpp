#include "harness.hpp"
#include "fastlob_books.hpp"

void run_fastlob() {
    bench_all<FastV1Book>("v1");
    bench_all<FastPagedBook>("paged");
}
