#include "external_check.hpp"

#include "bench/external/br_book.hpp"
#include "bench/external/fastlob_books.hpp"

TEST(ExternalBooks, BrAvlMatchesFastLobDepth) {
    expect_books_agree<FastV1Book, BrBook>("br-v1");
    expect_books_agree<FastPagedBook, BrBook>("br-paged");
}
