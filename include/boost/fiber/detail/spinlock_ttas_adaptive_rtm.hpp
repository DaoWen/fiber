
//          Copyright Oliver Kowalke 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_RTM_H
#define BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_RTM_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>

#include <boost/fiber/detail/config.hpp>
#include <boost/fiber/detail/cpu_relax.hpp>
#include <boost/fiber/detail/rtm.hpp>
#include <boost/fiber/detail/spinlock_status.hpp>
#include <boost/fiber/detail/spinlock_ttas_adaptive.hpp>

// based on informations from:
// https://software.intel.com/en-us/articles/benefitting-power-and-performance-sleep-loops
// https://software.intel.com/en-us/articles/long-duration-spin-wait-loops-on-hyper-threading-technology-enabled-intel-processors

namespace boost {
namespace fibers {
namespace detail {

class spinlock_ttas_adaptive_rtm {
private:
    spinlock_ttas_adaptive       splk_;

public:
    spinlock_ttas_adaptive_rtm() noexcept = default;

    spinlock_ttas_adaptive_rtm( spinlock_ttas_adaptive_rtm const&) = delete;
    spinlock_ttas_adaptive_rtm & operator=( spinlock_ttas_adaptive_rtm const&) = delete;

    void lock() noexcept {
        std::size_t collisions = 0 ;
        std::minstd_rand generator;
        for ( std::size_t retries = 0; retries < BOOST_FIBERS_RETRY_THRESHOLD; ++retries) {
            std::uint32_t status;
            if ( rtm_status::success == ( status = rtm_begin() ) ) {
                // add lock to read-set
                if ( spinlock_status::unlocked == splk_.state_.load( std::memory_order_acquire) ) {
                    // lock is free, enter critical section
                    return;
                }
                // lock was acquired by another thread
                // explicit abort of transaction with abort argument 'lock not free'
                rtm_abort_lock_not_free();
            }
            // transaction aborted
            if ( rtm_status::none != (status & rtm_status::may_retry) ) {
                // transaction may succeed on a retry
                cpu_relax();
            } else if ( rtm_status::none != (status & rtm_status::memory_conflict) ) {
                if ( BOOST_FIBERS_CONTENTION_WINDOW_THRESHOLD > collisions) {
                    // another logical processor conflicted with a memory address that was part or the read-/write-set
                    // at least two logical processors must have started a transaction the same time
                    std::uniform_int_distribution< std::size_t > distribution{
                        0, static_cast< std::size_t >( 1) << (std::min)(collisions, static_cast< std::size_t >( BOOST_FIBERS_CONTENTION_WINDOW_THRESHOLD)) };
                    const std::size_t z = distribution( generator);
                    ++collisions;
                    for ( std::size_t i = 0; i < z; ++i) {
                        cpu_relax();
                    }
                } else {
                    std::this_thread::yield();
                }
            } else if ( rtm_status::none != (status & rtm_status::explicit_abort) ) {
                // another logical processor has acquired the lock
                // wait till lock becomes free again
                std::size_t count = 0;
                while ( spinlock_status::locked == splk_.state_.load( std::memory_order_relaxed) ) {
                    if ( BOOST_FIBERS_SPIN_BEFORE_SLEEP0 > count) {
                        ++count;
                        cpu_relax();
                    } else if ( BOOST_FIBERS_SPIN_BEFORE_YIELD > count) {
                        ++count; 
                        static constexpr std::chrono::microseconds us0{ 0 };
                        std::this_thread::sleep_for( us0);
#if 0
                        using namespace std::chrono_literals;
                        std::this_thread::sleep_for( 0ms);
#endif
                    } else {
                        std::this_thread::yield();
                    }
                }
            } else {
                // transaction aborted due: 
                //  - internal buffer to track transactional state overflowed
                //  - debug exception or breakpoint exception was hit
                //  - abort during execution of nested transactions (max nesting limit exceeded)
                // -> use fallback path
                break;
            }
        }
        splk_.lock();
    }

    void unlock() noexcept {
        if ( spinlock_status::unlocked == splk_.state_.load( std::memory_order_acquire) ) {
            rtm_end();
        } else {
            splk_.unlock();
        }
    }
};

}}}

#endif // BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_RTM_H
