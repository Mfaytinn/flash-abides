// The kernel: one class, compiled once per event queue. Not a header and not a
// translation unit: sim.cpp includes it with T3_SIM = SimHeap, T3_QUEUE =
// EventHeap, T3_WHEEL = 0; again, as sim_wheel.cpp, with SimWheel, EventQueue
// and 1; as sim_linewheel.cpp with SimLineWheel, EventWheel and 2; and as
// sim_nswheel.cpp with SimNsWheel, NsWheel and 3. A run that keeps the heap
// executes none of a timing wheel's bookkeeping.
//
// T3_LEDGER is 1 in those four. sim_linewheel_lean.cpp and sim_nswheel_lean.cpp
// compile their wheel's kernel once more with T3_LEDGER = 0 (SimLineWheelLean,
// SimNsWheelLean), for runs whose ledger is not written: there a delivery only
// counts, and a send leaves out the three fields only a ledger row reads.
class T3_SIM {
public:
  T3_SIM(const Scenario &s, const SimOptions &o) : s_(s), opt_(o) {}
  // `arena` must outlive the Sim: its helper threads and rings live in it.
  SimResult run(Arena &arena);

private:
  void event_loop();
  // The smallest of the wake-up lanes' best key and the smallest representative
  // (RepSet::kNoKey when there is neither). Kept equal to that at the top of
  // every pass of the event loop, so the common event, one from the queue,
  // costs one compare against it. Refreshed only by events from the lanes or
  // representatives.
  u128 other_ = RepSet::kNoKey;
  void refresh_other() {
    u128 o = reps_.top_key();
    if (lanes_.any()) {
      const u128 b = lanes_.best_key();
      o = b < o ? b : o;
    }
    other_ = o;
  }
  uint32_t next_id() {
    if (msg_counter_ == 0xFFFFFFFFu) [[unlikely]]
      throw std::runtime_error("message id overflow");
    return ++msg_counter_;
  }

  // Constructs a message (new id) and sends it: log entry + event key.
  [[gnu::always_inline]] inline void send(uint64_t s, uint64_t r, Msg type,
                                          int64_t order_id, uint8_t side,
                                          int64_t price, int64_t qty,
                                          uint8_t flags, uint64_t extra) {
    const uint64_t sent = now_ + cdelay_[s] + extra;
    const uint64_t at = sent + latency_.next(s, r);
    const uint32_t id = next_id();
    LogEntry &e = log_.put(id);
#if T3_LEDGER // the send time, the scheduled delivery and the cause are read by
              // the ledger row alone
    e.t_sent = sent;
    e.t_sched = at;
#endif
    e.order_id = order_id;
    e.price = price;
    e.qty = qty;
#if T3_LEDGER
    e.causal = cur_causal_;
#endif
    e.type = static_cast<uint8_t>(type);
    e.side = side;
    e.flags = flags;
    heap_.push(make_key(at, s, r, id));
  }
  void send_plain(uint32_t s, uint32_t r, Msg type) {
    send(s, r, type, kNull, 0, 0, 0, 0, 0);
  }
  void report(uint32_t agent, Msg type, int64_t order_id, uint8_t side,
              int64_t price, int64_t qty) {
    send(0, agent, type, order_id, side, price, qty, 0, ack_delay_);
  }
  void set_wakeup(uint32_t a, uint64_t t) {
    if (t < now_) [[unlikely]]
      throw std::runtime_error(
          "set_wakeup() called with requested time not in future");
    heap_.push(
        make_key(t, a, a, next_id())); // wakeups take an id but no log entry
  }

  void trace_order(TraceType type, uint32_t agent, int64_t order_id,
                   uint8_t side, int64_t price, int64_t size) {
    const int64_t t = static_cast<int64_t>(now_);
    TraceRec &r = trace_.push(t);
    r.t = t;
    r.order_id = order_id;
    r.price = price;
    r.size = size;
    r.agent = static_cast<int32_t>(agent);
    r.type = static_cast<uint8_t>(type);
    r.side = side;
    r.pad = 0;
  }
  void trace_quote(uint8_t side, int64_t price, int64_t size) {
    const int64_t t = static_cast<int64_t>(now_);
    if (last_quote_t_[side] == t && last_quote_row_[side] != SIZE_MAX) {
      TraceRec &r = trace_.at(last_quote_row_[side]);
      r.price = price;
      r.size = size;
      return;
    }
    last_quote_row_[side] = trace_.size();
    last_quote_t_[side] = t;
    TraceRec &r = trace_.push(t);
    r.t = t;
    r.order_id = -1;
    r.price = price;
    r.size = size;
    r.agent = 0;
    r.type = static_cast<uint8_t>(TraceType::Quote);
    r.side = side;
    r.pad = 0;
  }

  // Recipient r's representative for the first message waiting in defer_[r]:
  // due when r is free.
  u128 rep_key(uint32_t r) {
    const uint64_t sid = defer_[r].front();
    rep_sid_[r] = sid;
    return make_key(busy_[r], static_cast<uint32_t>(sid >> 32), r,
                    static_cast<uint32_t>(sid));
  }
  [[gnu::always_inline]] inline void deliver(u128 k);
  void exchange_wakeup();
  // A delivered message's fields arrive as arguments: tsf is type | side << 8 |
  // flags << 16.
  [[gnu::always_inline]] inline void
  exchange_message(uint32_t sender, uint32_t tsf, int64_t order_id,
                   int64_t price, int64_t qty);
  [[gnu::noinline]] void handle_limit(uint32_t agent, int64_t order_id,
                                      uint8_t side, int64_t price, int64_t qty);
  bool book_cancel(int64_t order_id, uint8_t side, int64_t price,
                   uint32_t requester);
  [[gnu::noinline]] void agent_wakeup(uint32_t a);
  [[gnu::noinline]] void agent_message(uint32_t a, uint32_t tsf,
                                       int64_t order_id, int64_t price,
                                       int64_t qty);
  void act(uint32_t a, int64_t bid, int64_t ask, uint8_t flags);
  void place_limit(uint32_t a, uint8_t side, int64_t price, int64_t qty);

  const Scenario &s_;
  SimOptions opt_;
  uint64_t now_ = 0;
  uint64_t stop_ = 0, mkt_open_ = 0, mkt_close_ = 0;
  uint64_t ack_delay_ = 0, compute_delay_ = 0;
  uint32_t msg_counter_ = 0; // Message counter reset to 1: first id is 1
  uint32_t cur_causal_ = 0;
  int64_t order_counter_ = 0; // Order counter reset to 0

  NoiseStreams noise_;

  T3_QUEUE heap_;

  WakeLanes lanes_;
  bool use_lanes_ = false;
  LatencyStream latency_;
  Oracle oracle_;
  LegacyRandom master_;
  Book book_;
  std::vector<Agent> agents_;
  std::vector<AgentVecs> vecs_; // parallel to agents_
  std::vector<AgentCold> cold_; // parallel to agents_
  std::vector<uint64_t> busy_;
  std::vector<uint64_t> cdelay_;
  std::vector<uint32_t> close_subscribers_;
  // Per-recipient deferral (exact replacement for ABIDES's repeated busy
  // requeues): deferred entries wait in defer_[r] (smallest sender<<32|id
  // first), and r has exactly one representative in reps_, keyed (busy_[r] when
  // it was set, sender, r, id) for the front entry rep_sid_[r]. The run loop
  // takes the smallest of the event heap, the lanes and reps_ (t3/repset.hpp).
  std::vector<DeferQueue> defer_; // t3/deferqueue.hpp
  std::vector<uint64_t> rep_sid_;
  RepSet reps_;

  TraceStream trace_;
  MsgLog log_; // messages in flight
  // The market-close broadcast: one message object (one id) sent to every
  // subscriber. It stays out of the in-flight log; each recipient's scheduled
  // delivery time is kept here.
  uint32_t bcast_id_ = 0;
  LogEntry bcast_entry_{};
  std::vector<uint64_t> bcast_at_;
  size_t last_quote_row_[2] = {SIZE_MAX, SIZE_MAX};
  int64_t last_quote_t_[2] = {-1, -1};

  // --- cold: kept after the hot members so it does not shift their layout
  LedgerStream ledger_; // one row per delivery
  std::unique_ptr<Streamer>
      streamer_; // large runs: writes both files during the window
};

// -----------------------------------------------------------------------------
// exchange

void T3_SIM::exchange_wakeup() {
  if (now_ >= mkt_close_) {
    // One MarketClosePriceMsg object: one id (even with no subscribers), sent
    // to each. (mkt_close is after the exchange's only other wakeup, at the
    // start: one broadcast per run)
    if (bcast_id_)
      throw std::logic_error("second market-close broadcast");
    const uint32_t id = next_id();
    const uint64_t sent = now_ + cdelay_[0];
    bcast_entry_ = LogEntry{sent,
                            0,
                            kNull,
                            cur_causal_,
                            static_cast<uint8_t>(Msg::MarketClosePrice),
                            0,
                            0,
                            0,
                            0,
                            0};
    bcast_id_ = id;
    for (uint32_t agent : close_subscribers_) {
      const uint64_t at = sent + latency_.next(0, agent);
      bcast_at_[agent] =
          at; // (a later entry for the same recipient wins, as it always has)
      heap_.push(make_key(at, 0, agent, id));
    }
  }
}

inline void T3_SIM::exchange_message(uint32_t sender, uint32_t tsf,
                                     int64_t order_id, int64_t price,
                                     int64_t qty) {
  cdelay_[0] = compute_delay_;
  const Msg type = static_cast<Msg>(tsf & 0xFF);
  const uint8_t side = static_cast<uint8_t>(tsf >> 8);
  if (now_ > mkt_close_ && type != Msg::QuerySpread) {
    send_plain(0, sender, Msg::MarketClosed);
    return;
  }
  switch (type) {
  case Msg::LimitOrder:
    handle_limit(sender, order_id, side, price, qty);
    break;
  case Msg::QuerySpread: {
    uint8_t flags = 0;
    int64_t bid = 0, ask = 0;
    if (!book_.empty(0)) {
      flags |= kHasBid;
      bid = book_.best(0).price;
    }
    if (!book_.empty(1)) {
      flags |= kHasAsk;
      ask = book_.best(1).price;
    }
    if (now_ > mkt_close_)
      flags |= kClosed;
    send(0, sender, Msg::QuerySpreadResponse, kNull, 0, bid, ask, flags, 0);
    break;
  }
  case Msg::CancelOrder:
    book_cancel(order_id, side, price, sender);
    break;
  case Msg::MarketHoursRequest:
    cdelay_[0] = 0;
    send_plain(0, sender, Msg::MarketHours);
    break;
  case Msg::MarketClosePriceRequest:
    close_subscribers_.push_back(sender);
    break;
  default:
    throw std::logic_error("exchange received an unexpected message type");
  }
}

// OrderBook.cancel_order: the order must be resting in the level of its side
// and limit price.
bool T3_SIM::book_cancel(int64_t order_id, uint8_t side, int64_t price,
                         uint32_t requester) {
  if (order_id < 0 || order_id > int64_t{0xFFFFFFFE} || side > 1)
    return false;
  const size_t li = book_.find(side, price);
  if (li == Book::npos)
    return false;
  const uint32_t k =
      book_.find_order(side, li, static_cast<uint32_t>(order_id));
  if (k == kNone)
    return false;
  const int64_t remaining = book_.pool[k].qty;
  book_.unlink_at(k, li);
  report(requester, Msg::OrderCancelled, order_id, side, price, remaining);
  return true;
}

void T3_SIM::handle_limit(uint32_t agent, int64_t order_id, uint8_t side,
                          int64_t price, int64_t qty) {
  if (qty <= 0 || price < 0)
    return;
  const uint8_t opp = side ^ 1;
  std::vector<Level> &ov = book_.lv[opp];
  for (;;) {
    if (!ov.empty()) {
      Level &L = ov.back();
      const bool match = side == 0 ? price >= L.price : price <= L.price;
      if (match) {
        const uint32_t head = L.head;
        OrderNode &hn = book_.pool[head];
        if (s_.stp_enabled && hn.agent == agent) {
          if (s_.stp_cancel_oldest) {
            if (book_cancel(hn.id, hn.side, hn.price, hn.agent))
              continue;
          } else {
            report(agent, Msg::OrderCancelled, order_id, side, price, qty);
            break;
          }
        }
        // execute_order against the head of the best opposite level
        const int64_t fill_price = hn.price;
        const uint32_t rest_agent = hn.agent;
        const uint32_t rest_id = hn.id;
        int64_t q;
        if (qty >= hn.qty) {
          q = hn.qty;
          book_.unlink_best_head(opp, head);
        } else {
          q = qty;
          hn.qty -= q;
          L.total -= q;
        }
        report(rest_agent, Msg::OrderExecuted, rest_id, opp, fill_price, q);
        report(agent, Msg::OrderExecuted, order_id, side, fill_price, q);
        qty -= q;
        if (qty <= 0)
          break;
        continue;
      }
    }
    book_.append(side, price, static_cast<uint32_t>(order_id), qty,
                 static_cast<uint16_t>(agent));
    report(agent, Msg::OrderAccepted, order_id, side, price, qty);
    break;
  }
  if (!book_.empty(0))
    trace_quote(0, book_.best(0).price, book_.best(0).total);
  if (!book_.empty(1))
    trace_quote(1, book_.best(1).price, book_.best(1).total);
}

// -----------------------------------------------------------------------------
// agents

void T3_SIM::agent_wakeup(uint32_t a) {
  Agent &ag = agents_[a];
  if (ag.first_wake) [[unlikely]] {
    ag.first_wake = false;
    send_plain(a, 0, Msg::MarketClosePriceRequest);
  }
  if (!ag.knows_hours) [[unlikely]]
    send_plain(a, 0, Msg::MarketHoursRequest);
  if (!ag.knows_hours || ag.mkt_closed)
    return;
  if (use_lanes_) {
    lanes_.push(a, make_key(now_ + ag.interval, a, a, next_id()));
    if (lanes_.best_key() < other_)
      other_ = lanes_.best_key();
  } else {
    set_wakeup(a, now_ + ag.interval);
  }
  send_plain(a, 0, Msg::QuerySpread);
  ag.awaiting_spread = true;
  if (ag.type == AgentType::Noise)
    noise_.prefetch(ag.noise_slot); // consumed when the spread arrives
}

void T3_SIM::agent_message(uint32_t a, uint32_t tsf, int64_t order_id,
                           int64_t price, int64_t qty) {
  Agent &ag = agents_[a];
  const Msg type = static_cast<Msg>(tsf & 0xFF);
  const uint8_t side = static_cast<uint8_t>(tsf >> 8);
  const uint8_t flags = static_cast<uint8_t>(tsf >> 16);
  switch (type) {
  case Msg::QuerySpreadResponse:
    if (flags & kClosed)
      ag.mkt_closed = true;
    if (ag.awaiting_spread) {
      ag.awaiting_spread = false;
      if (!ag.mkt_closed)
        act(a, price, qty, flags);
    }
    return;
  case Msg::OrderAccepted:
    trace_order(TraceType::Accepted, a, order_id, side, price, qty);
    return;
  case Msg::OrderExecuted:
    trace_order(TraceType::PartialFill, a, order_id, side, price, qty);
    if (ag.type == AgentType::MarketMaker)
      vecs_[a].open.executed(order_id, qty);
    return;
  case Msg::OrderCancelled:
    trace_order(TraceType::Cancelled, a, order_id, side, price, qty);
    if (ag.type == AgentType::MarketMaker)
      vecs_[a].open.cancelled(order_id);
    return;
  case Msg::MarketHours:
    if (!ag.knows_hours) {
      ag.knows_hours = true;
      set_wakeup(a, mkt_open_ + 0);
    }
    return;
  case Msg::MarketClosePrice:
    return;
  case Msg::MarketClosed:
    ag.mkt_closed = true;
    return;
  default:
    throw std::logic_error("agent received an unexpected message type");
  }
}

void T3_SIM::place_limit(uint32_t a, uint8_t side, int64_t price, int64_t qty) {
  const int64_t id = order_counter_++; // LimitOrder construction takes an id
  if (qty <= 0) [[unlikely]]
    return;
  Agent &ag = agents_[a];
  if (ag.type == AgentType::MarketMaker)
    vecs_[a].open.push(OpenOrder{id, price, qty, side});
  send(a, 0, Msg::LimitOrder, id, side, price, qty, 0, 0);
  trace_order(TraceType::Submitted, a, id, side, price, qty);
}

void T3_SIM::act(uint32_t a, int64_t bid, int64_t ask, uint8_t sflags) {
  Agent &ag = agents_[a];
  AgentCold &cold = cold_[a];
  const AgentSpec &sp = cold.spec;
  // get_known_bid_ask returns None for an empty side; agents test truthiness (0
  // is falsy).
  const bool has_bid = (sflags & kHasBid) && bid != 0;
  const bool has_ask = (sflags & kHasAsk) && ask != 0;
  switch (ag.type) {
  case AgentType::Noise: {
    const NoiseDraw d =
        noise_.next(ag.noise_slot); // draw_noise() on the agent's RNG
    const int64_t size = d.size;
    const bool buy = d.buy();
    const int64_t offset = d.offset();
    // A buy rests at the ask (else the bid, else the reference price) plus the
    // offset, a sell at the bid (else the ask, else the reference price) minus
    // it. The side is a coin toss, so it is arithmetic and one call, not a
    // branch; the reference price (a cold line of its own per agent) is read
    // only when the book has neither side.
    int64_t base = buy ? (has_ask ? ask : bid) : (has_bid ? bid : ask);
    if (!(has_bid | has_ask)) [[unlikely]]
      base = sp.reference_price;
    const int64_t sell =
        -static_cast<int64_t>(!buy); // -1 for a sell, 0 for a buy
    place_limit(a, static_cast<uint8_t>(!buy), base + ((offset ^ sell) - sell),
                size); // base +/- offset
    return;
  }
  case AgentType::MarketMaker: {
    int64_t mid = sp.reference_price;
    if (has_bid && has_ask) {
      const int64_t sum = bid + ask;
      mid = sum >= 0 ? sum / 2 : -((-sum + 1) / 2); // Python floor division
    }
    vecs_[a].open.for_each([&](const OpenOrder &o) {
      send(a, 0, Msg::CancelOrder, o.id, o.side, o.price, o.qty, 0, 0);
    });
    const int64_t half = sp.spread_ticks / 2;
    for (int64_t lvl = 0; lvl < sp.depth_levels; ++lvl) {
      place_limit(a, 0, mid - half - lvl, sp.size_per_level);
      place_limit(a, 1, mid + half + lvl, sp.size_per_level);
    }
    return;
  }
  case AgentType::Value: {
    double mid;
    if (has_bid && has_ask)
      mid = (static_cast<double>(bid) + static_cast<double>(ask)) / 2.0;
    else if (has_bid)
      mid = static_cast<double>(bid);
    else if (has_ask)
      mid = static_cast<double>(ask);
    else
      return;
    const int64_t fund =
        oracle_.observe(static_cast<int64_t>(now_), cold.rng, sp.sigma_n);
    int64_t size = static_cast<int64_t>(py_round(sp.order_size_mean));
    if (size < 1)
      size = 1;
    if (mid < static_cast<double>(fund - sp.threshold_ticks) && has_ask)
      place_limit(a, 0, ask, size);
    else if (mid > static_cast<double>(fund + sp.threshold_ticks) && has_bid)
      place_limit(a, 1, bid, size);
    return;
  }
  case AgentType::Momentum: {
    double mid;
    if (has_bid && has_ask)
      mid = (static_cast<double>(bid) + static_cast<double>(ask)) / 2.0;
    else if (has_bid)
      mid = static_cast<double>(bid);
    else if (has_ask)
      mid = static_cast<double>(ask);
    else
      return;
    auto &h = vecs_[a].hist;
    h.push_back(mid);
    if (h.size() > static_cast<size_t>(sp.lookback) + 1)
      h.erase(h.begin());
    if (h.size() <= static_cast<size_t>(sp.lookback))
      return;
    const double past = h.front();
    int64_t size = static_cast<int64_t>(py_round(sp.order_size_mean));
    if (size < 1)
      size = 1;
    if (mid > past + static_cast<double>(sp.threshold_ticks) && has_ask)
      place_limit(a, 0, ask, size);
    else if (mid < past - static_cast<double>(sp.threshold_ticks) && has_bid)
      place_limit(a, 1, bid, size);
    return;
  }
  }
}

// -----------------------------------------------------------------------------
// delivery

inline void T3_SIM::deliver(u128 k) {
  const uint32_t r = key_r(k);
  const uint32_t id = key_id(k);
  const uint32_t s = key_s(k);
#if T3_LEDGER
  LedgerRow &row = ledger_.next();
  row.id = id;
  row.src = static_cast<uint16_t>(s);
  row.dst = static_cast<uint16_t>(r);
  cur_causal_ = id;
#else
  ledger_.count(); // no row: the ledger is not written
#endif
  if (s == r) { // wakeup
#if T3_LEDGER
    row.t_send = kNull;
    row.t_recv = static_cast<int64_t>(now_);
    row.order_id = kNull;
    row.causal = 0;
    row.type = static_cast<uint8_t>(Msg::Wakeup);
#endif
    busy_[r] = now_;
    if (r == 0)
      exchange_wakeup();
    else
      agent_wakeup(r);
    busy_[r] += cdelay_[r];
  } else {
    busy_[r] = now_ + cdelay_[r];
    const LogEntry *src;
    if (id != bcast_id_) [[likely]] {
      src = &log_.take(id);
#if T3_LEDGER
      std::memcpy(
          &row, src,
          32); // t_send, t_recv, order_id, causal, type (+3 ignored bytes)
#endif
    } else {
      src = &bcast_entry_;
#if T3_LEDGER
      std::memcpy(&row, src, 32);
      row.t_recv = static_cast<int64_t>(bcast_at_[r]);
#endif
    }
    // The fields are read into registers now: the callbacks reuse log slots.
    uint32_t tsf;
    std::memcpy(&tsf,
                reinterpret_cast<const char *>(src) + offsetof(LogEntry, type),
                sizeof tsf);
    tsf &= 0x00FFFFFFu;
    const int64_t order_id = src->order_id, price = src->price, qty = src->qty;
    if (r == 0)
      exchange_message(s, tsf, order_id, price, qty);
    else
      agent_message(r, tsf, order_id, price, qty);
  }
}

// -----------------------------------------------------------------------------
// run

// Event loop, separate from setup and diagnostics.
[[gnu::noinline]] void T3_SIM::event_loop() {
  const bool deferral = opt_.deferral;
  // Prefetch the next event's log entry on large rings. T3_NO_NEXT_PREFETCH
  // disables this.
  const bool prefetch_next =
      log_.ring() >= 4096 && !std::getenv("T3_NO_NEXT_PREFETCH");
  refresh_other();
  while (now_ <= stop_) {
    u128 k;
    // The next event: the smaller of the queue's top and `other_`, the smaller
    // of the lanes' best and the first representative (kNoKey, larger than any
    // key, when there is none).
    const u128 qk = heap_.top();
    const u128 ok = other_;
    bool from_rep = false;
    u128 rk = 0;
    if (qk < ok) [[likely]] {
      k = heap_.pop();
      // What follows is most likely the queue's new top: its log entry is asked
      // for now.
      if (prefetch_next)
        log_.prefetch(key_id(heap_.top()));
    } else if (ok == RepSet::kNoKey) {
      break;
    } else if (reps_.top_key() == ok) {
      from_rep = true;
      rk = ok;
    } else {
      k = lanes_.pop_best();
      refresh_other();
    }
    if (from_rep) { // the first message waiting for a busy recipient is due
      now_ = key_t(rk);
#if T3_WHEEL
      heap_.advance(now_);
#endif
      const uint32_t r = key_r(rk);
      if (busy_[r] >
          now_) { // r took a message directly since: due when it is free again
        reps_.replace_top(with_time(rk, busy_[r]));
        refresh_other();
        continue;
      }
      DeferQueue &q = defer_[r];
      q.pop_front();
      deliver(rk); // sends go to the heap and the lanes: r is still the top of
                   // reps_
      if (!q.empty())
        reps_.replace_top(rep_key(r));
      else
        reps_.erase_top();
      refresh_other();
      continue;
    }
    now_ = key_t(k);
#if T3_WHEEL
    heap_.advance(now_);
#endif
    const uint32_t r = key_r(k);
    if (busy_[r] > now_) {
      if (!deferral) {
        heap_.push(with_time(k, busy_[r]));
        continue;
      }
      const uint64_t sid = (uint64_t{key_s(k)} << 32) | key_id(k);
      defer_[r].push(sid);
      if (!reps_.has(r) || sid < rep_sid_[r]) {
        reps_.set(r, rep_key(r));
        refresh_other();
      }
      continue;
    }
    deliver(k);
  }
}

SimResult T3_SIM::run(Arena &arena) {
  const auto ts = std::chrono::steady_clock::now();
  const size_t n_agents = s_.agents.size() + 1;
  mkt_open_ = s_.mkt_open();
  mkt_close_ = s_.mkt_close();
  stop_ = s_.stop_time();
  ack_delay_ = static_cast<uint64_t>(s_.ack_delay_ns);
  compute_delay_ = static_cast<uint64_t>(s_.compute_delay_ns);
  now_ = kDateNs;

  // --- capacity estimate (growth still works; this only avoids faults in the
  // timed window)
  double msgs = 0, rows = 0, orders = 0;
  for (const AgentSpec &a : s_.agents) {
    const double w = static_cast<double>(s_.horizon_ns) /
                         static_cast<double>(a.interval_ns) +
                     2;
    if (a.type == AgentType::MarketMaker) {
      msgs += w * (8.0 * static_cast<double>(a.depth_levels) + 4);
      rows += w * (7.0 * static_cast<double>(a.depth_levels) + 2);
      orders +=
          w * 2.0 *
          static_cast<double>(a.depth_levels); // act(): two orders per level
    } else {
      msgs += w * 7;
      rows += w * 5;
      orders += w; // act(): at most one order
    }
  }
  // T3_EST_SCALE (tests only) shrinks the estimate to exercise growth during
  // the window.
  static const double est_scale = std::getenv("T3_EST_SCALE")
                                      ? std::atof(std::getenv("T3_EST_SCALE"))
                                      : 1.0;
  msgs *= est_scale;
  rows *= est_scale;
  orders *= est_scale;
  const size_t est_msgs = static_cast<size_t>(std::min(4e8, msgs * 1.1)) + 4096;
  const size_t est_rows = static_cast<size_t>(std::min(4e8, rows * 1.1)) + 4096;
  // Keep tiny runs inline. T3_HELPER_MIN_MSGS tunes the helper threshold;
  // the value sequence is identical with and without helpers.
  static const size_t helper_min =
      std::getenv("T3_HELPER_MIN_MSGS")
          ? std::strtoull(std::getenv("T3_HELPER_MIN_MSGS"), nullptr, 10)
          : 8192;
  const bool helpers = est_msgs >= helper_min;
  // Draw helpers require enough acts to reach the ring at a block boundary
  // (NoiseStreams::kBlock acts). T3_DRAW_MIN_ACTS tunes the threshold.
  size_t n_noise = 0;
  double noise_acts = 0, max_noise_acts = 0;
  for (const AgentSpec &a : s_.agents) {
    if (a.type != AgentType::Noise)
      continue;
    const double w = static_cast<double>(s_.horizon_ns) /
                         static_cast<double>(a.interval_ns) +
                     2;
    ++n_noise;
    noise_acts += w;
    max_noise_acts = std::max(max_noise_acts, w);
  }
  static const double draw_min =
      std::getenv("T3_DRAW_MIN_ACTS")
          ? std::atof(std::getenv("T3_DRAW_MIN_ACTS"))
          : 2048;
  const bool draw_thread =
      opt_.draw_thread && helpers && !std::getenv("T3_NO_DRAW_THREAD") &&
      max_noise_acts >= static_cast<double>(NoiseStreams::kBlock) &&
      noise_acts >= draw_min;
  const bool latency_thread = opt_.latency_thread && helpers;

  // Large runs stream both files during the window; others write them after
  // the loop. T3_STREAM_MIN tunes the estimated-delivery threshold.
  static const size_t stream_min =
      std::getenv("T3_STREAM_MIN")
          ? std::strtoull(std::getenv("T3_STREAM_MIN"), nullptr, 10)
          : size_t{1} << 14;
  // Below 4 CPUs the Streamer shares a CPU and costs time, but a whole-mode run
  // holds ~150 bytes per row: from 2^22 estimated deliveries on, bounded memory
  // wins.
  const bool stream = opt_.stream && est_msgs >= stream_min &&
                      (opt_.threads >= 4 || est_msgs >= (size_t{1} << 22));
  const size_t group = stream ? Streamer::group_for(est_msgs) : 0;
  // Size the message ring from estimated events in flight, not agent count.
  // Entries still in flight when their slots wrap move to a side table.
  static const size_t log_ring_env =
      std::getenv("T3_LOG_RING")
          ? std::strtoull(std::getenv("T3_LOG_RING"), nullptr, 10)
          : 0;
  const size_t log_ring =
      log_ring_env
          ? log_ring_env
          : std::clamp<size_t>(8 * static_cast<size_t>(events_in_flight(s_)),
                               1024, size_t{1} << 17);

  // Streamed runs: the fixed-size buffers the window touches come from one
  // populated mapping, made before any helper thread starts (Arena). Whole-mode
  // runs keep them on the heap: freed when the Sim is destroyed, their
  // already-faulted pages then serve the output encoder's buffers. T3_NO_ARENA
  // restores separate zeroed allocations.
  const bool use_arena = stream && opt_.prefault &&
                         !std::getenv("T3_NO_PREFAULT") &&
                         !std::getenv("T3_NO_ARENA");
  // A ledger the card does not require is not written, and neither is an
  // optional one estimated at twice its size limit or more (the limit still
  // applies, byte by byte, to one estimated below that). A skipped ledger has
  // no ring and keeps no rows.
  const bool ledger_skip =
      opt_.ledger_skip ||
      (stream && opt_.ledger_cap &&
       static_cast<double>(est_msgs) * kLedgerBytesPerDelivery >
           2.0 * static_cast<double>(opt_.ledger_cap));
  const size_t arena_bytes =
      !use_arena
          ? 0
          : MsgLog::arena_bytes(log_ring) +
                LatencyStream::arena_bytes(s_, latency_thread) +
                NoiseStreams::arena_bytes(n_noise, draw_thread) +
                (stream ? Streamer::arena_bytes(group, !ledger_skip) : 0);
  // Huge pages for the arena from a million estimated deliveries on
  // (T3_NO_ARENA_HUGE: A/B).
  static const bool arena_huge_allowed =
      std::getenv("T3_NO_ARENA_HUGE") == nullptr;
  arena.init(arena_bytes, arena_huge_allowed && est_msgs >= kArenaHugeMsgs);
  Arena *const ar = use_arena ? &arena : nullptr;
  // The message log before the Streamer: its thread populates the arena's
  // carve-outs taken so far, and the log is the first thing the event loop
  // writes.
  log_.init(log_ring, !opt_.prefault, ar);
  const auto ts_arena = std::chrono::steady_clock::now();
  const size_t est_nodes =
      static_cast<size_t>(std::min(4e8, orders * 1.02)) + 256; // order ids
  // The Streamer first: its thread faults in its memory while this thread seeds
  // the agents. The ledger on a thread of its own from a million estimated
  // deliveries: below that a run is over in tens of milliseconds and one thread
  // keeps up. T3_STREAMER_SPLIT=0|1 forces it.
  static const int split_env = std::getenv("T3_STREAMER_SPLIT")
                                   ? std::atoi(std::getenv("T3_STREAMER_SPLIT"))
                                   : -1;
  const bool split_ledger =
      split_env >= 0 ? split_env != 0 : est_msgs >= (size_t{1} << 20);
  if (stream)
    streamer_ = std::make_unique<Streamer>(
        ledger_, trace_, opt_.ledger_path, opt_.trace_path, est_rows, est_nodes,
        group, ar, ledger_skip, opt_.ledger_cap, split_ledger);
  const auto ts_streamer = std::chrono::steady_clock::now();

  // --- config.py seeding order
  master_.reseed(static_cast<uint32_t>(s_.seed));
  const uint32_t oracle_seed = master_.seed_draw();
  oracle_.init(s_, &master_, oracle_seed);
  (void)master_.seed_draw(); // exchange random_state (unused)
  agents_.resize(n_agents);
  vecs_.resize(n_agents);
  cold_.resize(n_agents);
  for (size_t i = 1; i < n_agents; ++i) {
    Agent &a = agents_[i];
    AgentCold &c = cold_[i];
    c.spec = s_.agents[i - 1];
    a.type = c.spec.type;
    a.interval = c.spec.interval_ns;
    c.rng.reseed(master_.seed_draw());
  }
  const auto ts_agents = std::chrono::steady_clock::now();
  latency_.init(s_, master_.seed_draw(), latency_thread, ar);
  (void)master_.seed_draw(); // kernel random_state (unused)
  {
    std::vector<std::pair<NoiseParams, LegacyRandom *>> noise;
    for (size_t i = 1; i < n_agents; ++i) {
      Agent &a = agents_[i];
      if (a.type != AgentType::Noise)
        continue;
      a.noise_slot = static_cast<uint16_t>(noise.size());
      AgentCold &c = cold_[i];
      noise.push_back(
          {NoiseParams{c.spec.order_size_mean, c.spec.order_size_std,
                       c.spec.price_offset_ticks},
           &c.rng});
    }
    noise_.init(std::move(noise), draw_thread, ar);
  }
  const auto ts_seeded = std::chrono::steady_clock::now();

  {
    std::vector<uint64_t> intervals(n_agents, 0);
    for (size_t i = 1; i < n_agents; ++i)
      intervals[i] = agents_[i].interval;
    use_lanes_ =
        opt_.lanes && !std::getenv("T3_NO_LANES") && lanes_.init(intervals);
  }
  busy_.assign(n_agents, kDateNs);
  cdelay_.assign(n_agents, 50);
  defer_.clear();
  defer_.resize(
      n_agents); // not assign(): a queue owns its window and is not copied
  // With more than eight makers, defer queues stay sorted arrays.
  // T3_DEFER_WINDOW=0|1 overrides window use.
  size_t n_makers = 0;
  for (const AgentSpec &a : s_.agents)
    n_makers += a.type == AgentType::MarketMaker;
  bool defer_window = n_makers <= 8;
  if (const char *e = std::getenv("T3_DEFER_WINDOW"))
    defer_window = std::atoi(e) != 0;
  if (!defer_window)
    for (DeferQueue &q : defer_)
      q.use_window(false);
  rep_sid_.assign(n_agents, 0);
  reps_.reserve(n_agents);

  const bool pf = opt_.prefault && !std::getenv("T3_NO_PREFAULT");
  if (!stream) { // an estimate can be far too high (it saturates at 4e8): bound
                 // what it reserves
    if (opt_.ledger_skip)
      ledger_.init_skipped();
    else
      ledger_.init_whole(std::min(est_msgs, kWholeCapRows),
                         pf ? std::min(est_msgs, kWholePrefaultRows) : 0);
    trace_.init_whole(std::min(est_rows, kWholeCapRows),
                      pf ? std::min(est_rows, kWholePrefaultRows) : 0);
  }
  // At most a few hundred orders rest at once on every public unit; the pool
  // grows if needed. A maker's order is cancelled one quote later, so the id
  // table spans several quotes of all makers (eight times their resting
  // orders).
  book_.reserve(
      std::min<size_t>(est_nodes, 1024),
      static_cast<uint32_t>(std::clamp<size_t>(
          8 * resting_maker_orders(s_), Book::kMinIdSlots, Book::kMaxIdSlots)));
#if T3_WHEEL
  {
    size_t slots = 0;
#if T3_WHEEL == 3
    ns_wheel_geometry(s_, slots);
    heap_.init(slots);
#else
    unsigned shift = 0;
#if T3_WHEEL == 1
    wheel_geometry(s_, shift, slots);
#else
    line_wheel_geometry(s_, shift, slots);
#endif
    heap_.init(shift, slots);
#endif
    heap_.advance(now_);
  }
#endif
  heap_.reserve(1024 + n_agents * 4); // it grows if a run needs more
  // Touch everything the window will write to avoid first-touch page faults.
  for (DeferQueue &q : defer_)
    prepare(q.storage(), 16);
  for (size_t i = 1; i < n_agents; ++i) {
    Agent &a = agents_[i];
    AgentCold &c = cold_[i];
    if (a.type == AgentType::MarketMaker)
      prepare(
          vecs_[i].open.storage(),
          4 * static_cast<size_t>(std::max<int64_t>(0, c.spec.depth_levels)) +
              16);
    if (a.type == AgentType::Momentum) { // at most one entry per act, whatever
                                         // the lookback
      const uint64_t acts = s_.horizon_ns / a.interval + 2;
      const uint64_t want =
          static_cast<uint64_t>(std::max<int64_t>(0, c.spec.lookback)) + 2;
      prepare(vecs_[i].hist, static_cast<size_t>(std::min(
                                 {want, acts + 2, uint64_t{1} << 20})));
    }
  }
  prepare(close_subscribers_, n_agents);
  prepare(book_.lv[0], 128);
  prepare(book_.lv[1], 128);
  bcast_at_.assign(n_agents, 0);
  const auto ts_reserved = std::chrono::steady_clock::now();

  // --- kernel.initialize(): exchange kernel_initializing ->
  // set_wakeup(mkt_close) (id 1); kernel_starting -> set_wakeup(start) for the
  // exchange, then every agent.
  set_wakeup(0, mkt_close_);
  for (size_t i = 0; i < n_agents; ++i)
    set_wakeup(static_cast<uint32_t>(i), kDateNs);
  const auto ts_wakeups = std::chrono::steady_clock::now();

  // T3_PIN_LOOP=<cpu> (measurement only): the event loop runs on that CPU alone
  // for the window. Started under `taskset` on the other CPUs, the helper
  // threads inherit that set and this thread leaves it, so the loop has a core
  // to itself whatever the scheduler would have done.
  static const int pin_cpu =
      std::getenv("T3_PIN_LOOP") ? std::atoi(std::getenv("T3_PIN_LOOP")) : -1;
  cpu_set_t cpus_before;
  bool pinned = false;
  if (pin_cpu >= 0 &&
      ::sched_getaffinity(0, sizeof cpus_before, &cpus_before) == 0) {
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(pin_cpu, &one);
    pinned = ::sched_setaffinity(0, sizeof one, &one) == 0;
  }

#if !T3_LEDGER
  // This kernel fills no ledger row: it must only ever run a ledger that is not
  // written.
  if (!ledger_.skipped())
    throw std::logic_error("the lean kernel was given a ledger to write");
#endif

  // --- the timed simulation window
  latency_.go();
  noise_.go();
  if (streamer_)
    streamer_->go();
  static const bool phases = std::getenv("T3_PHASES") != nullptr;
  struct rusage ru0{}, rt0{};
  if (phases)
    ::getrusage(RUSAGE_SELF, &ru0), ::getrusage(RUSAGE_THREAD, &rt0);
  const auto t0 = std::chrono::steady_clock::now();
  event_loop();
  const auto t1 = std::chrono::steady_clock::now();
  if (pinned)
    ::sched_setaffinity(0, sizeof cpus_before, &cpus_before);
  if (phases) {
    struct rusage ru1{}, rt1{};
    ::getrusage(RUSAGE_SELF, &ru1), ::getrusage(RUSAGE_THREAD, &rt1);
    std::fprintf(stderr,
                 "  window: %ld minor faults (%ld on the event loop), %ld vol "
                 "/ %ld invol ctx switches "
                 "(%ld / %ld on the event loop)\n",
                 ru1.ru_minflt - ru0.ru_minflt, rt1.ru_minflt - rt0.ru_minflt,
                 ru1.ru_nvcsw - ru0.ru_nvcsw, ru1.ru_nivcsw - ru0.ru_nivcsw,
                 rt1.ru_nvcsw - rt0.ru_nvcsw, rt1.ru_nivcsw - rt0.ru_nivcsw);
    std::fprintf(stderr, "  huge pages: %ld MB held (arena %s)\n",
                 std::max(Arena::anon_huge_kb(), 0L) >> 10,
                 arena.huge() ? "asked for them" : "did not ask");
  }
  latency_.stop_thread();
  noise_.stop();
  const auto ts_ledger = std::chrono::steady_clock::now();
  if (streamer_)
    streamer_->finish();

#if T3_WHEEL == 1
  if (std::getenv("T3_PHASES"))
    std::fprintf(stderr,
                 "  queue: wheel %u ns x %zu slots, pushes %zu to the wheel "
                 "(mean %.1f entries in the slot), %zu beyond the window, %zu "
                 "spilled from full slots\n",
                 1u << heap_.shift(), heap_.slots(), heap_.wheel_pushes(),
                 heap_.mean_occupancy(), heap_.far_pushes(), heap_.spills());
#elif T3_WHEEL >= 2
  if (std::getenv("T3_PHASES")) {
    std::fprintf(stderr, "  queue: %s wheel%s %llu ns x %zu slots",
                 T3_WHEEL == 2 ? "line" : "ns",
                 T3_LEDGER ? "" : " (no ledger rows)", 1ull << heap_.shift(),
                 heap_.slots());

    std::fprintf(stderr, "\n");
  }
#else
  if (std::getenv("T3_PHASES"))
    std::fprintf(stderr, "  queue: heap\n");
#endif
  if (std::getenv("T3_PHASES")) {
    std::fprintf(stderr, "  waiting queues: %s (%zu market makers)\n",
                 defer_window ? "by id when long" : "sorted arrays", n_makers);
    std::fprintf(stderr,
                 "  latency: switched to ring at block %lld, consumed %llu "
                 "blocks, stalls %llu\n",
                 static_cast<long long>(latency_.switch_block()),
                 static_cast<unsigned long long>(latency_.blocks_consumed()),
                 static_cast<unsigned long long>(latency_.stalls()));
    // How busy each helper thread was while the event loop ran (100%: it cannot
    // keep up).
    const double w = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::fprintf(stderr,
                 "  helper threads during the window: streamer %.0f%% (%.1f "
                 "ms), ledger thread %.0f%% (%.1f ms), "
                 "latency draws %.0f%%, noise draws %.0f%% of %.1f ms\n",
                 streamer_ ? 100.0 * streamer_->window_cpu_ms() / w : 0.0,
                 streamer_ ? streamer_->window_cpu_ms() : 0.0,
                 streamer_ ? 100.0 * streamer_->ledger_cpu_ms() / w : 0.0,
                 streamer_ ? streamer_->ledger_cpu_ms() : 0.0,
                 100.0 * latency_.producer_cpu_ms() / w,
                 100.0 * noise_.producer_cpu_ms() / w, w);
  }

  SimResult res;
  res.sim_seconds = std::chrono::duration<double>(t1 - t0).count();
  res.setup_seconds = std::chrono::duration<double>(t0 - ts).count();
  if (std::getenv("T3_PHASES")) {
    auto ms = [](auto a, auto b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::fprintf(stderr,
                 "  setup: arena %.3f ms (%.1f MiB), streamer start %.3f ms, "
                 "seeding %zu agents %.3f ms, "
                 "latency+noise streams %.3f ms, reserve %.3f ms, wakeups %.3f "
                 "ms, go %.3f ms\n",
                 ms(ts, ts_arena), static_cast<double>(arena_bytes) / 1048576.0,
                 ms(ts_arena, ts_streamer), n_agents - 1,
                 ms(ts_streamer, ts_agents), ms(ts_agents, ts_seeded),
                 ms(ts_seeded, ts_reserved), ms(ts_reserved, ts_wakeups),
                 ms(ts_wakeups, t0));
  }
  if (std::getenv("T3_PHASES"))
    std::fprintf(
        stderr,
        "  streams: %s (trace groups %zu, ring grows %zu, waits %zu), finish "
        "%.3f ms, "
        "log spills %zu (ring %zu), orders %lld (est %zu), ledger %s\n",
        streamer_ ? "streamed" : "whole",
        streamer_ ? streamer_->trace_groups() : 0,
        streamer_ ? streamer_->ring_grows() : 0,
        streamer_ ? streamer_->waits() : 0,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts_ledger)
            .count(),
        log_.spills(), log_.ring(), static_cast<long long>(order_counter_),
        est_nodes, streamer_ ? streamer_->ledger_fate() : "written");
  if (std::getenv("T3_PHASES") && streamer_) {
    const double *f = streamer_->finish_parts();
    std::fprintf(stderr,
                 "  trace finish: last groups %.2f ms, marks + msg_type chunks "
                 "%.2f ms (%.0f threads), write %.2f ms\n",
                 f[0], f[2], f[1], f[3]);
  }
  res.est_msgs = est_msgs;
  res.est_rows = est_rows;
  res.n_trace_rows = trace_.size();
  res.n_deliveries = ledger_.size();
  res.streamed = streamer_ != nullptr;
  if (res.streamed) {
    res.trace_sha256 = streamer_->trace_sha256();
    res.ledger_fate = streamer_->ledger_fate();
  } else {
    res.trace = std::move(trace_.rows());
    res.ledger = std::move(ledger_.rows());
  }
  res.n_order_ids = order_counter_;
  return res;
}
