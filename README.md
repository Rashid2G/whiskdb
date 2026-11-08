# WhiskDB

> Database with Superpowers — In-Engine Code Execution with Real-Time Persistence.

WhiskDB is an experimental database engine designed to eliminate the network boundary between application logic and database storage by co-locating execution and state in shared memory.

> [!NOTE]
> WhiskDB is currently under active pre-MVP development as a Senior Capstone Project (YZL 400) at **OSTİM Technical University**.

---

## Core Objectives

- **In-Engine Code Execution**: Run application business logic directly inside the database process.
- **Zero-Copy In-Memory Access**: High-performance access to data structures without serialization overhead.
- **Durable Persistence**: Reliable crash recovery and write-ahead persistence.
- **Real-Time Subscriptions**: WebSocket-based push updates for connected clients.

---

## Documentation

Full documentation is available via MkDocs:

```bash
pip install mkdocs
mkdocs serve
```

Visit the `docs/` directory for architecture notes, coding conventions, and developer guidelines.

---

## Team:

- **Abdullah Al Tamsha**
- **Ibrahim Jamil Karaja**
- **Rashid Alkatheri**

