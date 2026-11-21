#include "external_check.hpp"

#include "bench/external/fastlob_books.hpp"
#include "bench/external/liquibook_book.hpp"

TEST(ExternalBooks, LiquibookMatchesFastLobDepth) {
    expect_books_agree<FastV1Book, LiquibookBook>("liquibook-v1");
    expect_books_agree<FastPagedBook, LiquibookBook>("liquibook-paged");
    expect_crossing_books_agree<FastPagedBook, LiquibookBook>("liquibook-paged-cross");
}
