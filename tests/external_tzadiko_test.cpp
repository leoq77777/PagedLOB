#include "external_check.hpp"

#include "bench/external/fastlob_books.hpp"
#include "bench/external/tzadiko_book.hpp"

TEST(ExternalBooks, TzadikoMatchesFastLobDepth) {
    expect_books_agree<FastV1Book, TzadikoBook>("tzadiko-v1");
    expect_books_agree<FastPagedBook, TzadikoBook>("tzadiko-paged");
}
