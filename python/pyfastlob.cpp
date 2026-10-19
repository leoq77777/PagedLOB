#include "fastlob/execution_sim.hpp"
#include "fastlob/lob_fast.hpp"
#include "fastlob/synthetic_mbo.hpp"
#include "fastlob/types.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <vector>

namespace py = pybind11;

namespace {

struct SnapshotBuffer {
    std::vector<std::int64_t> data;
    std::size_t rows{0};
};

class PyOrderBook {
public:
    explicit PyOrderBook(std::size_t max_orders = 1 << 20)
        : book_(std::make_unique<fastlob::FastOrderBook>(max_orders, max_orders / 8)) {}

    bool add(std::uint64_t id, int side, std::int64_t price, std::uint32_t qty) {
        return book_->add(id, static_cast<fastlob::Side>(side), price, qty);
    }

    bool cancel(std::uint64_t id) { return book_->cancel(id); }

    bool modify(std::uint64_t id, std::uint32_t qty) { return book_->modify(id, qty); }

    bool execute(std::uint64_t id, std::uint32_t qty) { return book_->execute(id, qty); }

    std::size_t order_count() const { return book_->order_count(); }

    SnapshotBuffer snapshot(std::size_t n) const {
        SnapshotBuffer buf;
        buf.rows = n;
        buf.data.resize(n * 4);
        (void)book_->snapshot_top_n(n, buf.data);
        return buf;
    }

    fastlob::FastOrderBook& native() { return *book_; }

private:
    std::unique_ptr<fastlob::FastOrderBook> book_;
};

class PyExecutionSimulator {
public:
    explicit PyExecutionSimulator(PyOrderBook& book) : sim_(book.native()) {}

    bool submit_limit(std::uint64_t id, int side, std::int64_t price, std::uint32_t qty) {
        return sim_.submit_limit(id, static_cast<fastlob::Side>(side), price, qty);
    }

    py::object submit_market(std::uint64_t id, int side, std::uint32_t qty) {
        auto fr = sim_.submit_market(id, static_cast<fastlob::Side>(side), qty);
        if (!fr) {
            return py::none();
        }
        py::dict d;
        d["order_id"] = fr->strategy_order_id;
        d["price"] = fr->price;
        d["qty"] = fr->qty;
        d["fully_filled"] = fr->fully_filled;
        return d;
    }

    void on_public_trade(int resting_side, std::int64_t price, std::uint32_t qty) {
        sim_.on_public_trade(static_cast<fastlob::Side>(resting_side), price, qty);
    }

    std::size_t fill_count() const { return sim_.fills().size(); }

private:
    fastlob::ExecutionSimulator sim_;
};

}  // namespace

PYBIND11_MODULE(pyfastlob, m) {
    m.doc() = "FastLOB Python bindings (zero-copy snapshot buffer export)";
    m.attr("SIDE_BUY") = 0;
    m.attr("SIDE_SELL") = 1;
    m.attr("TICK_SIZE_FACTOR") = fastlob::kTickSizeFactor;

    m.def("dollars_to_ticks", &fastlob::dollars_to_ticks);
    m.def("ticks_to_dollars", &fastlob::ticks_to_dollars);

    py::class_<SnapshotBuffer>(m, "SnapshotBuffer", py::buffer_protocol())
        .def_buffer([](SnapshotBuffer& b) -> py::buffer_info {
            return py::buffer_info(
                b.data.data(),
                sizeof(std::int64_t),
                py::format_descriptor<std::int64_t>::format(),
                2,
                {b.rows, std::size_t{4}},
                {sizeof(std::int64_t) * 4, sizeof(std::int64_t)});
        })
        .def_property_readonly("rows", [](const SnapshotBuffer& b) { return b.rows; })
        .def("to_list", [](const SnapshotBuffer& b) {
            py::list rows;
            for (std::size_t i = 0; i < b.rows; ++i) {
                py::list row;
                row.append(b.data[i * 4 + 0]);
                row.append(b.data[i * 4 + 1]);
                row.append(b.data[i * 4 + 2]);
                row.append(b.data[i * 4 + 3]);
                rows.append(row);
            }
            return rows;
        });

    py::class_<PyOrderBook>(m, "OrderBook")
        .def(py::init<std::size_t>(), py::arg("max_orders") = (1 << 20))
        .def("add", &PyOrderBook::add)
        .def("cancel", &PyOrderBook::cancel)
        .def("modify", &PyOrderBook::modify)
        .def("execute", &PyOrderBook::execute)
        .def("order_count", &PyOrderBook::order_count)
        .def("snapshot", &PyOrderBook::snapshot, py::arg("n") = 10);

    py::class_<PyExecutionSimulator>(m, "ExecutionSimulator")
        .def(py::init<PyOrderBook&>(), py::keep_alive<1, 2>())
        .def("submit_limit", &PyExecutionSimulator::submit_limit)
        .def("submit_market", &PyExecutionSimulator::submit_market)
        .def("on_public_trade", &PyExecutionSimulator::on_public_trade)
        .def("fill_count", &PyExecutionSimulator::fill_count);

    m.def(
        "generate_synthetic_mbo",
        [](std::size_t n, std::uint64_t seed) {
            fastlob::SyntheticConfig cfg;
            cfg.num_events = n;
            cfg.seed = seed;
            auto events = fastlob::generate_synthetic_mbo(cfg);
            py::list out;
            for (const auto& e : events) {
                py::dict d;
                d["type"] = static_cast<int>(e.type);
                d["order_id"] = e.order_id;
                d["side"] = static_cast<int>(e.side);
                d["price"] = e.price;
                d["qty"] = e.qty;
                d["ts"] = e.ts;
                out.append(d);
            }
            return out;
        },
        py::arg("num_events") = 1000, py::arg("seed") = 42);
}
