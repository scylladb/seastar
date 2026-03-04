IO scheduler uses rate-limiter to throttle the amount of data it dispatches into the disk.

# Basic math

The scheduler's main equation that models the disk behavior is

$$ \frac {bandwidth_r} {bandwidth_{r_{max}}} +
   \frac {bandwidth_w} {bandwidth_{w_{max}}} +
   \frac {iops_r} {iops_{r_{max}}} +
   \frac {iops_w} {iops_{w_{max}}} \le 1.0 $$

Let's say that

$$ m_b = \frac {bandwidth_{r_{max}}} {bandwidth_{w_{max}}} \ \ m_o = \frac {iops_{r_{max}}} {iops_{w_{max}}} $$

Since

$$ bandwidth_x = \frac {d}{dt} bytes_x \ \ iops_x = \frac {d}{dt} ios_x $$

The main equation turns into

$$ \frac {d}{dt} \( \frac {bytes_r + m_b \times bytes_w} {bandwidth_{r_{max}}} + \frac {ios_r + m_o \times ios_w} {iops_{r_{max}}} \) \le 1.0 $$

Requests are asigned a 2d value of _{1, bytes}_ for reads and _{m<sub>o</sub>, m<sub>b</sub> * bytes}_ for writes called "tickets"

The "normalization" operation is defined as

$$ N(ticket) = \frac {ticket_0}{iops_{r_{max}}} + \frac {ticket_1}{bandwidth_{r_{max}}} $$

With that the main equation turns into

$$ \frac {d}{dt} \sum_{ticket} N(ticket) \le 1.0 $$

The time-derivative limitation is then implemented with the token bucket algorithm

# Token bucket

The algorithm creates token bucket with the refill rate of _1.0_ and each request wants to carry the fractional token value of _tokens = N(ticket)_ with _ticket_ defined above

The bucket additionally requires the _limit_ parameter which is the maximum number of tokens the bucket may hold. This value is calculated using the _io_latency_goal_ parameter, to be the amount of tokens accumulated for the _io_latency_goal_ duration

# Slowing down the flow

Let's assume we need to reduce the rate of request run-time. This means that we want to

$$ \frac {d}{dt} \( \frac {bytes_r + m_b \times bytes_w} {bandwidth_{r_{max}} \times \alpha} + \frac {ios_r + m_o \times ios_w} {iops_{r_{max}} \times \beta} \) \le 1.0 \ \  \alpha \le 1.0 \ \beta \le 1.0 $$

There are many ways of selecting which of IOPS or bandwidth to reduce, the "general" slowing down may assume that

$$ \alpha = \beta = \frac {1}{\gamma} , \ \ \ \gamma \ge 1.0 $$

The main equation then turns into

$$ \frac {d}{dt} \sum_{ticket} \gamma \times N(ticket) \le 1.0 $$

which in turn means, that we can just multiply the request cost (in tokens) by some number above 1.0

# Detecting the slowdown

Let's say that

_d<sub>i</sub>_ -- the amount of requests dispatched at tick _i_
_p<sub>i</sub>_ -- the amount of requests processed by disk at tick _i_
_c<sub>i</sub>_ -- the amount of requests completed by reactor loop at tick _i_

We can observe _d<sub>i</sub>_ and _c<sub>i</sub>_ in the dispatcher, but _not_ the _p<sub>i</sub>_, because we don't have direct access to disks' queues

After _n_ ticks we have

_D<sub>n</sub>_ -- total amount of requests dispatched,
_P<sub>n</sub>_ -- total amount of requests processed,
_C<sub>n</sub>_ -- total amount of requests completed,

$$ D_n = \sum_{i=0}^n d_i $$

$$ P_n = \sum_{i=0}^n p_i $$

$$ C_n = \sum_{i=0}^n c_i $$

* Disk cannot process more than it was dispatched, but it can process less "accumulating" a queue

$$ d_i \ge p_i   \Rightarrow  D_n \ge P_n$$

* Reactor cannot complete more than it was processed by disk either

$$ p_i \ge c_i \Rightarrow P_n \ge C_n $$


Note that _D<sub>n</sub> > P<sub>n</sub>_  means that disk is falling behind. Our goal is to make sure the disk doesn't do it and doesn't accumulate the queue, i.e. _D<sub>n</sub> = P<sub>n</sub>_, but we cannot observe _P<sub>n</sub>_ directly, only _C<sub>n</sub>_.

Next

$$ D_n - P_n = Qd_n \ge 0 $$

$$ P_n - C_n = Qc_n \ge 0 $$

$$ D_n - C_N = Qd_n + Qc_n \ge 0 $$

_Qd<sub>n</sub>_ is the queue accumulated in disk. We try to avoid it. _Qc<sub>n</sub>_ is the accumulated delta between processed (by disk) and completed (by reactor) requests. We cannot reliably say that it goes to zero over time, because there's always some unknown amount of processed but not yet completed requests. So the above formula contains two unknowns and we cannot solve it. Let's try the other way

$$ \frac {D_n} {P_n} = Rd_n \ge 1 $$

$$ \frac {P_n} {C_n} = Rc_n \ge 1 $$

$$ \frac {D_n} {C_n} = Rd_n \times Rc_n \ge 1 $$

_Rd<sub>n</sub>_ is the ratio of dispatched to processed requests. If it's 1, we're OK, if it's greater, disk is accumulating the queue, we try to avoid it. _Rc<sub>n</sub>_ is the ratio between processed (by disk) and completed (by reactor) requests. It has a not immediately apparent, but very pretty property

$$ \lim_{n\to\infty} Rc_n = \lim_{n\to\infty} \frac {P_n} {C_n} = \lim_{n\to\infty} \frac {C_n + Qc_n} { C_n} = \lim_{n\to\infty} \(1 +  \frac {Qc_n} {C_n} \) = 1 + \lim_{n\to\infty} \frac {Qc_n} {C_n}$$

The _Qc<sub>n</sub>_ doesn't grow over time. It's non-zero, but it's upper bound by some value. Respectively

$$ \lim_{n\to\infty} Rc_n = 1 $$

$$ \lim_{n\to\infty} \frac {D_n} {C_n} = \lim_{n\to\infty} \( Rd_n \times Rc_n \) =  \lim_{n\to\infty} Rd_n  $$

IOW -- we can say if the disk is accumulating the queue or not by observing the dispatched-to-completed (to _completed_, not _processed_) over a long enough time
