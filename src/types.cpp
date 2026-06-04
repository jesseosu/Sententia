#include "sententia/types.hpp"

namespace sententia {

const char* toString(Side s) noexcept {
    switch (s) {
        case Side::Buy:
            return "BUY";
        case Side::Sell:
            return "SELL";
    }
    return "?";
}

const char* toString(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit:
            return "LIMIT";
        case OrderType::Market:
            return "MARKET";
    }
    return "?";
}

const char* toString(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::GoodTillCancel:
            return "GTC";
        case TimeInForce::ImmediateOrCancel:
            return "IOC";
    }
    return "?";
}

const char* toString(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:
            return "NONE";
        case RejectReason::DuplicateOrderId:
            return "DUPLICATE_ORDER_ID";
        case RejectReason::UnknownOrderId:
            return "UNKNOWN_ORDER_ID";
        case RejectReason::ZeroQuantity:
            return "ZERO_QUANTITY";
        case RejectReason::NonPositivePrice:
            return "NON_POSITIVE_PRICE";
        case RejectReason::WrongInstrument:
            return "WRONG_INSTRUMENT";
        case RejectReason::MarketOrderMustBeIoc:
            return "MARKET_ORDER_MUST_BE_IOC";
    }
    return "?";
}

}  // namespace sententia
