# Project Development

!!! note "Scope of this Page"
    This page is focuses on the development progress. and the state of the project.

---

## Project Scope & Objectives

1. **Sandboxed Code Execution**
2. **Zero-Copy In-Memory Access**
3. **Persistence & Crash Recovery**
4. **Real-Time Client Subscriptions**

---

## External Libraries

WhiskDB integrates external libraries to support its architecture and objectives:

- **[Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/)** (`boost/beast`): Networking and WebSocket protocol library built on top of **Boost.Asio**, powering WhiskDB's client connection handling and real-time subscription streaming.

For detailed developer guidelines and conventions, see the [Developer Guide](developer-guide.md#external-libraries).

---
