#include "sententia/event.hpp"

#include <sstream>

namespace sententia {
namespace {

struct HeaderVisitor {
    template <typename T>
    const EventHeader& operator()(const T& e) const noexcept {
        return e.header;
    }
};

void writeHeader(std::ostringstream& os, const EventHeader& h) {
    os << "seq=" << h.eventSeq << " cmd=" << h.commandSeq << ' ';
}

struct RenderVisitor {
    std::ostringstream& os;

    void operator()(const OrderAccepted& e) const {
        writeHeader(os, e.header);
        os << "ACCEPTED id=" << e.id << " side=" << toString(e.side) << " type=" << toString(e.type)
           << " tif=" << toString(e.tif) << " px=" << e.price << " qty=" << e.quantity;
    }

    void operator()(const OrderRejected& e) const {
        writeHeader(os, e.header);
        os << "REJECTED id=" << e.id << " reason=" << toString(e.reason);
    }

    void operator()(const Trade& e) const {
        writeHeader(os, e.header);
        os << "TRADE aggressor=" << e.aggressorId << " resting=" << e.restingId
           << " side=" << toString(e.aggressorSide) << " px=" << e.price << " qty=" << e.quantity;
    }

    void operator()(const OrderResting& e) const {
        writeHeader(os, e.header);
        os << "RESTING id=" << e.id << " side=" << toString(e.side) << " px=" << e.price
           << " qty=" << e.quantity;
    }

    void operator()(const OrderCancelled& e) const {
        writeHeader(os, e.header);
        os << "CANCELLED id=" << e.id << " side=" << toString(e.side) << " px=" << e.price
           << " qty=" << e.remaining << " by=" << (e.engineInitiated ? "ENGINE" : "CLIENT");
    }

    void operator()(const CancelRejected& e) const {
        writeHeader(os, e.header);
        os << "CANCEL_REJECTED id=" << e.id << " reason=" << toString(e.reason);
    }

    void operator()(const TopOfBookChanged& e) const {
        writeHeader(os, e.header);
        os << "TOB bid=";
        if (e.hasBid) {
            os << e.bidPrice << 'x' << e.bidQuantity;
        } else {
            os << "none";
        }
        os << " ask=";
        if (e.hasAsk) {
            os << e.askPrice << 'x' << e.askQuantity;
        } else {
            os << "none";
        }
    }
};

}  // namespace

const EventHeader& header(const Event& e) noexcept {
    return std::visit(HeaderVisitor{}, e);
}

std::string toString(const Event& e) {
    std::ostringstream os;
    std::visit(RenderVisitor{os}, e);
    return os.str();
}

}  // namespace sententia
