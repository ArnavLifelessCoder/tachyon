#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "me/matching_engine.hpp"
#include "me/order_protocol.hpp"

namespace {

using me::OrderType;
using me::Side;

// Runs requests against one engine and returns the events as protocol lines,
// which keeps expectations short and readable.
class Engine : public ::testing::Test {
 protected:
  me::MatchingEngine engine;

  template <class Request>
  std::vector<std::string> run(const Request& request) {
    std::vector<std::string> out;
    engine.submit(request, [&](const me::ExecEvent& e) { out.push_back(me::protocol::format(e)); });
    return out;
  }
  std::vector<std::string> buy(std::uint64_t id, std::uint32_t price, std::uint32_t qty,
                               OrderType type = OrderType::limit) {
    return run(me::NewOrder{id, Side::buy, price, qty, type});
  }
  std::vector<std::string> sell(std::uint64_t id, std::uint32_t price, std::uint32_t qty,
                                OrderType type = OrderType::limit) {
    return run(me::NewOrder{id, Side::sell, price, qty, type});
  }
  using Lines = std::vector<std::string>;
};

TEST_F(Engine, RestingLimitOrdersAckWithoutTrades) {
  EXPECT_EQ(buy(1, 99, 10), (Lines{"A 1 99 10"}));
  EXPECT_EQ(sell(2, 101, 10), (Lines{"A 2 101 10"}));
  EXPECT_EQ(engine.resting(), 2U);
  EXPECT_EQ(engine.book().bids().best()->price, 99U);
  EXPECT_EQ(engine.book().asks().best()->price, 101U);
}

TEST_F(Engine, PartialFillLeavesMakerRemainder) {
  sell(1, 100, 100);
  EXPECT_EQ(buy(2, 100, 30), (Lines{"A 2 100 30", "T 2 1 100 30"}));
  EXPECT_EQ(engine.find(1)->qty, 70U);
  EXPECT_EQ(engine.find(2), nullptr);  // fully filled taker never rests
}

TEST_F(Engine, TakerRemainderRestsAtItsLimit) {
  sell(1, 100, 10);
  EXPECT_EQ(buy(2, 101, 25), (Lines{"A 2 101 25", "T 2 1 100 10"}));  // maker price
  ASSERT_NE(engine.find(2), nullptr);
  EXPECT_EQ(engine.find(2)->qty, 15U);
  EXPECT_EQ(engine.book().bids().best()->price, 101U);
  EXPECT_EQ(engine.book().asks().best(), nullptr);
}

TEST_F(Engine, WalksSeveralLevelsBestFirst) {
  sell(1, 102, 30);
  sell(2, 100, 10);
  sell(3, 101, 20);
  EXPECT_EQ(buy(4, 102, 45), (Lines{"A 4 102 45", "T 4 2 100 10", "T 4 3 101 20", "T 4 1 102 15"}));
  EXPECT_EQ(engine.find(1)->qty, 15U);
  EXPECT_EQ(engine.book().asks().size(), 1U);
}

TEST_F(Engine, TimePriorityWithinALevel) {
  buy(1, 50, 5);
  buy(2, 50, 5);
  buy(3, 50, 5);
  EXPECT_EQ(sell(4, 50, 12), (Lines{"A 4 50 12", "T 4 1 50 5", "T 4 2 50 5", "T 4 3 50 2"}));
  EXPECT_EQ(engine.find(3)->qty, 3U);
}

TEST_F(Engine, LimitDoesNotTradeThroughItsPrice) {
  sell(1, 100, 10);
  sell(2, 105, 10);
  EXPECT_EQ(buy(3, 103, 30), (Lines{"A 3 103 30", "T 3 1 100 10"}));
  EXPECT_EQ(engine.find(2)->qty, 10U);
  EXPECT_EQ(engine.find(3)->qty, 20U);
}

TEST_F(Engine, IocRemainderIsCancelled) {
  sell(1, 100, 10);
  sell(2, 101, 10);
  EXPECT_EQ(buy(3, 100, 25, OrderType::ioc), (Lines{"A 3 100 25", "T 3 1 100 10", "X 3 100 15"}));
  EXPECT_EQ(engine.find(3), nullptr);
  EXPECT_EQ(engine.find(2)->qty, 10U);
}

TEST_F(Engine, IocWithNothingToMatchIsFullyCancelled) {
  sell(1, 100, 10);
  EXPECT_EQ(buy(2, 99, 5, OrderType::ioc), (Lines{"A 2 99 5", "X 2 99 5"}));
  EXPECT_EQ(engine.resting(), 1U);
}

TEST_F(Engine, MarketOrderOnEmptyBookIsRejected) {
  EXPECT_EQ(buy(1, 0, 10, OrderType::market), (Lines{"R 1 no_liquidity"}));
  buy(2, 50, 10);  // same-side liquidity does not count
  EXPECT_EQ(buy(3, 0, 10, OrderType::market), (Lines{"R 3 no_liquidity"}));
  EXPECT_EQ(engine.resting(), 1U);
}

TEST_F(Engine, MarketOrderWalksAnyPriceAndCancelsRemainder) {
  buy(1, 90, 5);
  buy(2, 10, 5);
  EXPECT_EQ(sell(3, 0, 12, OrderType::market),
            (Lines{"A 3 0 12", "T 3 1 90 5", "T 3 2 10 5", "X 3 0 2"}));
  EXPECT_EQ(engine.resting(), 0U);
}

TEST_F(Engine, CancelRemovesRemainingQuantity) {
  sell(1, 100, 10);
  buy(2, 100, 4);
  EXPECT_EQ(run(me::CancelOrder{1}), (Lines{"X 1 100 6"}));
  EXPECT_EQ(run(me::CancelOrder{1}), (Lines{"R 1 unknown_order"}));
  EXPECT_EQ(engine.book().asks().size(), 0U);
}

TEST_F(Engine, ModifyReducingQuantityKeepsPriority) {
  sell(1, 100, 10);
  sell(2, 100, 10);
  EXPECT_EQ(run(me::ModifyOrder{1, 100, 4}), (Lines{"A 1 100 4"}));
  EXPECT_EQ(buy(3, 100, 6), (Lines{"A 3 100 6", "T 3 1 100 4", "T 3 2 100 2"}));
}

TEST_F(Engine, ModifyIncreasingQuantityLosesPriority) {
  sell(1, 100, 10);
  sell(2, 100, 10);
  EXPECT_EQ(run(me::ModifyOrder{1, 100, 15}), (Lines{"A 1 100 15"}));
  EXPECT_EQ(buy(3, 100, 12), (Lines{"A 3 100 12", "T 3 2 100 10", "T 3 1 100 2"}));
}

TEST_F(Engine, ModifyPriceChangeLosesPriorityAndCanMatch) {
  sell(1, 101, 10);
  sell(2, 100, 10);
  buy(3, 99, 5);
  // Moving order 1 to 100 puts it behind order 2 at that level.
  run(me::ModifyOrder{1, 100, 10});
  EXPECT_EQ(buy(4, 100, 12), (Lines{"A 4 100 12", "T 4 2 100 10", "T 4 1 100 2"}));
  // Moving the bid through the offer trades immediately as a re-entered limit.
  EXPECT_EQ(run(me::ModifyOrder{3, 100, 5}), (Lines{"A 3 100 5", "T 3 1 100 5"}));
  EXPECT_EQ(engine.find(1)->qty, 3U);
}

TEST_F(Engine, ModifyToSameValuesIsANoOpAck) {
  sell(1, 100, 10);
  sell(2, 100, 10);
  EXPECT_EQ(run(me::ModifyOrder{1, 100, 10}), (Lines{"A 1 100 10"}));
  EXPECT_EQ(engine.book().asks().best()->head->id, 1U);
}

TEST_F(Engine, RejectsInvalidRequests) {
  EXPECT_EQ(buy(1, 100, 0), (Lines{"R 1 invalid_quantity"}));
  EXPECT_EQ(buy(1, 0, 10), (Lines{"R 1 invalid_price"}));
  EXPECT_EQ(buy(1, 0, 10, OrderType::ioc), (Lines{"R 1 invalid_price"}));
  buy(1, 100, 10);
  EXPECT_EQ(sell(1, 200, 10), (Lines{"R 1 duplicate_id"}));
  EXPECT_EQ(run(me::ModifyOrder{9, 100, 1}), (Lines{"R 9 unknown_order"}));
  EXPECT_EQ(run(me::ModifyOrder{1, 100, 0}), (Lines{"R 1 invalid_quantity"}));
  EXPECT_EQ(run(me::ModifyOrder{1, 0, 5}), (Lines{"R 1 invalid_price"}));
  EXPECT_EQ(engine.find(1)->qty, 10U);
}

TEST_F(Engine, FilledIdCanBeReused) {
  sell(1, 100, 5);
  buy(2, 100, 5);
  EXPECT_EQ(sell(1, 100, 5), (Lines{"A 1 100 5"}));
}

TEST_F(Engine, BookStaysValidAndResetEmptiesIt) {
  for (std::uint64_t i = 0; i < 200; ++i) {
    run(me::NewOrder{i, i % 2 ? Side::buy : Side::sell, static_cast<std::uint32_t>(95 + i % 11),
                     static_cast<std::uint32_t>(1 + i % 7), OrderType::limit});
    ASSERT_TRUE(engine.book().valid());
  }
  engine.reset();
  EXPECT_EQ(engine.resting(), 0U);
  EXPECT_EQ(engine.book().bids().size() + engine.book().asks().size(), 0U);
}

}  // namespace
