---
title: Distributed Transactions
linkTitle: Distributed Transactions
description: Distributed ACID Transactions
menu:
  v1.2:
    identifier: architecture-distributed-acid-transactions
    parent: architecture-acid-transactions
    weight: 1153
isTocNested: false
showAsideToc: true
---

Distributed ACID transactions are transactions modifying multiple rows in more than one shard. YugaByte DB supports distributed transactions, enabling features such as strongly consistent secondary indexes and multi-table/row ACID operations in both the YCQL context as well as in the YSQL context (currently in Beta). This section provides some common concepts and notions used in YugaByte's approach to implementing distributed transactions.  Once you are familiar with these concepts, please see the [Core Functions / IO Path with Distributed Transactions](../transactional-io-path/) section for a walk-through of a distributed transaction lifecycle.

## Provisional records

Just as YugaByte DB stores values written by single-shard ACID transactions into
[DocDB](../../concepts/docdb/persistence/), it needs to store uncommitted values written by
distributed transactions in a similar persistent data structure. However, we cannot just write them
to DocDB as regular values, because they would then become visible at different times to clients
reading through different tablet servers, allowing a client to see a partially applied transaction
and thus breaking atomicity.  YugaByte DB therefore writes *provisional records* to all tablets
responsible for the keys the transaction is trying to modify. We call them "provisional" as opposed
to "regular" ("permanent") records, because they are invisible to readers until the transaction
commits.

Provisional records are stored in a separate part of the RocksDB key space in the same RocksDB /
DocDB instance as regular records, but with a separate prefix that puts all provisional records'
RocksDB keys before those of regular records. Compared to other possible design options, such as
storing provisional records inline with the regular records or putting them in a separate RocksDB
instance altogether, the approach we have chosen has the following benefits:

  - It is easy to scan all provisional records. As we will see, this is very helpful in cleaning up
    aborted / abandoned transactions.
  - During the read path, we need to handle provisional records very differently compared to regular
    records, and putting them in a separate section of the RocksDB key space allows to simplify the
    read path.
  - Storing provisional records in the same RocksDB instance allows to atomically delete provisional
    records and write regular records as one RocksDB operation after the transaction is committed.

### Encoding details of provisional records

There are three types of RocksDB key/value pairs corresponding to provisional records, omitting
the one-byte prefix that puts these records before all regular records in RocksDB.

![DocDB storage, including provisional records](/images/architecture/txn/provisional_record_storage.svg)

#### 1. Primary provisional records

  ```
  DocumentKey, SubKey1, ..., SubKeyN, LockType, ProvisionalRecordHybridTime -> TxnId, Value
  ```

  The `DocumentKey`, `SubKey1`, ..., `SubKey` components exactly match those in DocDB's
  [encoding](../../concepts/docdb/persistence/#mapping-docdb-documents-to-rocksdb) of "paths" to
  a particular subdocument (e.g. a row, a column, or an element in a collection-type column) to
  RocksDB keys.

  Each of these primary provisional records also acts as a persistent revocable lock. There are some
  similarities as well as differences when compared to [blocking in-memory
  locks](../isolation-levels/) maintained by every tablet's lock
  manager. These persistent locks can be of any of the same types as for in-memory leader-only locks
  (SI write, serializable write/read, and a separate "strong"/"weak" classification for handling
  nested document changes).  However, unlike the leader-side in-memory locks, the locks represented
  by provisional records can be revoked by another conflicting transaction.  The conflict resolution
  subsystem makes sure that for any two conflicting transactions, at least one of them is aborted.

  As an example, suppose a snapshot isolation transaction is setting column `col1` in row `row1` to
  `value1`. Then `DocumentKey` is `row1` and `SubKey1` is `col1`. Suppose the provisional record was
  written into the tablet with hybrid `1516847525206000`, and the transaction id is
  `7c98406e-2373-499d-88c2-25d72a4a178c`. In that case we will end up with the following provisional
  record values in RocksDB:

  ```
  row1, WeakSIWrite, 1516847525206000 -> 7c98406e-2373-499d-88c2-25d72a4a178c
  row1, col1, StrongSIWrite, 1516847525206000 -> 7c98406e-2373-499d-88c2-25d72a4a178c, value1
  ```

  We can see that we are using `WeakSIWrite` lock type for the row (the "parent" of the column we
  are writing), and `StrongSIWrite` for the column itself. The provisional record for the column is
  also where the column's value being written by the transaction is stored.

#### 2. Transaction metadata records

- `TxnId -> StatusTabletId, IsolationLevel, Priority`

    - `StatusTabletId` is the id of the tablet that keeps track of this transaction's status.
      Unlike the case of tables/tablets holding user data, where we are using a [hash-based
      mapping](../../concepts/sharding/) from keys to tablets, there is no deterministic way
      to compute the transaction status tablet id by transaction id, so this information must be
      explicitly passed to all components handling a particular transaction.

    - `Isolation Level` [Snapshot Isolation](https://en.wikipedia.org/wiki/Snapshot_isolation) or
      [Serializable Isolation](https://en.wikipedia.org/wiki/Serializability). 

    - `Priority` This priority is assigned randomly during transaction creation. When a conflict
      is detected between two transactions, the transaction with lower priority is
      aborted and restarted.

#### 3. Provisional record keys indexed by transaction id** ("reverse index")
```
TxnId, HybridTime -> primary provisional record key
```

  This mapping allows us to find all RocksDB records belonging to a particular transaction.  This is
  being used when cleaning up committed or aborted transactions. Note that because multiple RocksDB
  key/value pairs belonging to primary privisonal records can we written for the same transaction
  with the same hybrid time, we need to use an increasing counter (which we call a *write id*) at
  the end of the encoded representation of hybrid time in order to obtain unique RocksDB keys for
  this reverse index. This write id is shown as `.0`, `.1`, etc. in `T130.0`, `T130.1` in the figure
  above.


## Transaction status tracking

Atomicity (the "A" in "ACID") means that either all values written by a transaction are visible, or
none are visible at all. YugaByte DB already provides atomicity of single-shard updates by
replicating them via Raft and applying them as one write batch to the underlying RocksDB / DocDB
storage engine. The same approach could be reused to make *transaction status* changes atomic.  The status of transactions is tracked in a "transaction status" table. This table, under the covers, is just another elastic/sharded table in the system. The transaction id (a globally unique id) serves as the key in the table, and updates to a transaction's status are simple single-shard ACID operations. This allows us to atomically make all values written as part of that transaction visible by setting the status to "committed" in that transaction's status record in the table.

A transaction status record contains the following fields for a particular transaction id:

  * **Status** (*pending*, *committed*, or *aborted*).

    All transactions start in the "pending" status, and progress to "committed" or "aborted" status,
    in which they remain permanently until cleaned up.

After a transaction is committed, two more fields are set:

  * **Commit hybrid timestamp**. This timestamp is chosen as the current hybrid time at the
    transaction status tablet at the moment of appending the "transaction committed" entry to its
    Raft log. It is then used as the final MVCC timestamp for regular records that replace the
    transaction's provisional records when provisional records are being applied and cleaned up.

  * **List of ids of participating tablets**. After a transaction commits, we know the final set of
    tablets that the transaction has written to. The tablet server managing the transaction sends
    this list to the transaction status tablet as part of the commit message, and the transaction
    status tablet makes sure that all participating tablets are notified of the transaction's
    committed status. This process might take multiple retries, and the transaction record can only
    be cleaned up after this is done.


## See also

To continue exploring the architecture of YugaByte DB's distributed transaction implementation,
please take a look at the [Core Functions / IO Path with Distributed Transactions](../transactional-io-path/) section next.
