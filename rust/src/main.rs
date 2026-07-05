//! Pure-Rust reproducer (no Python) for the lost TCP reset (RST) on macOS
//! loopback, using the raw `kqueue`/`kevent` and socket syscalls via `libc`.
//!
//! Mirrors the deferred-close pattern that reproduces most often: a `kevent`
//! poll cycle runs between deleting the client fd's filters and abortively
//! closing it (SO_LINGER {1,0} + close).
//!
//! Each iteration:
//!   1. Establish a loopback client + accepted server socket, both non-blocking.
//!   2. Register the client (EVFILT_READ + EVFILT_WRITE); fill via write-
//!      readiness until send() blocks against the non-reading server.
//!   3. EV_DELETE the client's filters, run one `kevent` poll cycle, then
//!      abortively close it.
//!   4. Register the server (EVFILT_READ); drain the buffered data, then wait
//!      for the disconnect (EOF / errno).
//!   5. If the disconnect never arrives within TIMEOUT, the reset was lost.
//!
//! macOS / *BSD only. Usage: `repro [SECONDS]` (default 300).

use std::mem;
use std::ptr;
use std::time::{Duration, Instant};

const TIMEOUT: Duration = Duration::from_secs(1);

unsafe fn errno() -> i32 {
    *libc::__error()
}

unsafe fn set_nonblocking(fd: i32) {
    let flags = libc::fcntl(fd, libc::F_GETFL, 0);
    libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK);
}

unsafe fn loopback_addr(port: u16) -> libc::sockaddr_in {
    let mut a: libc::sockaddr_in = mem::zeroed();
    a.sin_len = mem::size_of::<libc::sockaddr_in>() as u8;
    a.sin_family = libc::AF_INET as u8;
    a.sin_port = port.to_be();
    a.sin_addr.s_addr = 0x7f00_0001u32.to_be(); // 127.0.0.1
    a
}

/// A connected loopback (client, server) fd pair, both non-blocking.
unsafe fn make_pair() -> (i32, i32) {
    let lsock = libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0);
    let one: i32 = 1;
    libc::setsockopt(
        lsock,
        libc::SOL_SOCKET,
        libc::SO_REUSEADDR,
        &one as *const _ as *const libc::c_void,
        mem::size_of::<i32>() as u32,
    );
    let addr = loopback_addr(0);
    libc::bind(
        lsock,
        &addr as *const _ as *const libc::sockaddr,
        mem::size_of::<libc::sockaddr_in>() as u32,
    );
    libc::listen(lsock, 1);

    let mut bound: libc::sockaddr_in = mem::zeroed();
    let mut blen = mem::size_of::<libc::sockaddr_in>() as u32;
    libc::getsockname(lsock, &mut bound as *mut _ as *mut libc::sockaddr, &mut blen);

    let client = libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0);
    libc::connect(
        client,
        &bound as *const _ as *const libc::sockaddr,
        mem::size_of::<libc::sockaddr_in>() as u32,
    );
    let server = libc::accept(lsock, ptr::null_mut(), ptr::null_mut());
    libc::close(lsock);

    set_nonblocking(client);
    set_nonblocking(server);
    (client, server)
}

unsafe fn kev_change(kq: i32, fd: i32, filter: i16, flags: u16) {
    let ev = libc::kevent {
        ident: fd as usize,
        filter,
        flags,
        fflags: 0,
        data: 0,
        udata: ptr::null_mut(),
    };
    libc::kevent(kq, &ev, 1, ptr::null_mut(), 0, ptr::null());
}

/// Poll for up to 16 events; `timeout` of 0 duration is an immediate poll.
unsafe fn kev_poll(kq: i32, timeout: Duration) -> Vec<libc::kevent> {
    let mut events: [libc::kevent; 16] = mem::zeroed();
    let ts = libc::timespec {
        tv_sec: timeout.as_secs() as libc::time_t,
        tv_nsec: timeout.subsec_nanos() as libc::c_long,
    };
    let n = libc::kevent(kq, ptr::null(), 0, events.as_mut_ptr(), 16, &ts);
    if n <= 0 {
        return Vec::new();
    }
    events[..n as usize].to_vec()
}

unsafe fn describe(fd: i32) -> String {
    let mut sa: libc::sockaddr_in = mem::zeroed();
    let mut slen = mem::size_of::<libc::sockaddr_in>() as u32;
    let peer = if libc::getpeername(fd, &mut sa as *mut _ as *mut libc::sockaddr, &mut slen) == 0 {
        format!("getpeername=port {}", u16::from_be(sa.sin_port))
    } else {
        format!("getpeername->errno {}", errno())
    };
    let mut so: i32 = 0;
    let mut solen = mem::size_of::<i32>() as u32;
    libc::getsockopt(
        fd,
        libc::SOL_SOCKET,
        libc::SO_ERROR,
        &mut so as *mut _ as *mut libc::c_void,
        &mut solen,
    );
    let mut b = [0u8; 1];
    let pk = libc::recv(fd, b.as_mut_ptr() as *mut libc::c_void, 1, libc::MSG_PEEK);
    let peek = if pk < 0 {
        format!("errno {}", errno())
    } else if pk == 0 {
        "EOF".to_string()
    } else {
        format!("{}bytes", pk)
    };
    format!("{} SO_ERROR={} MSG_PEEK={}", peer, so, peek)
}

/// Returns `Some(diagnostics)` if the abort was NOT delivered (the hang),
/// `None` if it was delivered.
unsafe fn one_iteration(kq: i32, buf: &mut [u8]) -> Option<String> {
    let (client, server) = make_pair();
    let chunk = [b'x'; 65536];

    // Register the client for read+write and fill until send() blocks.
    kev_change(kq, client, libc::EVFILT_READ, libc::EV_ADD);
    kev_change(kq, client, libc::EVFILT_WRITE, libc::EV_ADD);
    let mut filled = false;
    let fill_deadline = Instant::now() + TIMEOUT;
    while !filled && Instant::now() < fill_deadline {
        for ev in kev_poll(kq, Duration::from_millis(500)) {
            if ev.ident == client as usize && ev.filter == libc::EVFILT_WRITE {
                loop {
                    let r = libc::send(
                        client,
                        chunk.as_ptr() as *const libc::c_void,
                        chunk.len(),
                        0,
                    );
                    if r < 0 {
                        break; // EWOULDBLOCK
                    }
                }
                kev_change(kq, client, libc::EVFILT_WRITE, libc::EV_DELETE);
                filled = true;
            }
        }
    }
    if !filled {
        kev_change(kq, client, libc::EVFILT_READ, libc::EV_DELETE);
        libc::close(client);
        libc::close(server);
        return None; // couldn't fill; skip
    }

    // Deferred abort: delete the client's remaining filter, run one poll cycle,
    // then abortively close it.
    kev_change(kq, client, libc::EVFILT_READ, libc::EV_DELETE);
    kev_poll(kq, Duration::ZERO); // <-- the poll cycle between unregister and close
    let linger = libc::linger {
        l_onoff: 1,
        l_linger: 0,
    };
    libc::setsockopt(
        client,
        libc::SOL_SOCKET,
        libc::SO_LINGER,
        &linger as *const _ as *const libc::c_void,
        mem::size_of::<libc::linger>() as u32,
    );
    libc::close(client);

    // Register the server; drain buffered data, then wait for the disconnect.
    kev_change(kq, server, libc::EVFILT_READ, libc::EV_ADD);
    let mut outcome: Option<String> = Some(String::new()); // filled below if undelivered
    let deadline = Instant::now() + TIMEOUT;
    let mut done = false;
    while !done {
        let now = Instant::now();
        if now >= deadline {
            outcome = Some(describe(server));
            break;
        }
        for ev in kev_poll(kq, deadline - now) {
            if ev.ident == server as usize && ev.filter == libc::EVFILT_READ {
                let r = libc::recv(server, buf.as_mut_ptr() as *mut libc::c_void, buf.len(), 0);
                if r < 0 || r == 0 {
                    outcome = None; // delivered (ECONNRESET / EOF)
                    done = true;
                    break;
                }
                // else: drained buffered data; keep waiting
            }
        }
    }

    libc::close(server); // auto-removes its kevents
    outcome
}

fn main() {
    let run_seconds: f64 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(300.0);
    println!("rust repro: raw select.kqueue/kevent via libc");
    println!("run={run_seconds}s timeout={:?} (deferred close)\n", TIMEOUT);

    unsafe {
        let kq = libc::kqueue();
        let mut buf = vec![0u8; 1 << 20];
        let mut delivered: u64 = 0;
        let mut undelivered: u64 = 0;
        let start = Instant::now();
        while start.elapsed().as_secs_f64() < run_seconds {
            match one_iteration(kq, &mut buf) {
                Some(desc) => {
                    undelivered += 1;
                    println!("  [{}] UNDELIVERED: {}", delivered + undelivered, desc);
                }
                None => delivered += 1,
            }
        }
        libc::close(kq);

        let total = delivered + undelivered;
        println!("\ndelivered   = {delivered}/{total}");
        println!("UNDELIVERED = {undelivered}/{total}");
        if undelivered > 0 {
            std::process::exit(1);
        }
    }
}
