---
title: Comparisons
linkTitle: Comparisons
description: Comparisons
image: /images/section_icons/index/comparisons.png
headcontent: This page highlights how YugaByte DB compares against other operational databases in the distributed SQL and NoSQL categories. Click on the database name in the table header to see a more detailed comparison.
aliases:
  - /comparisons/
section: FAQ
menu:
  latest:
    identifier: comparisons
    weight: 2720
---

## Distributed SQL databases

Feature | [CockroachDB](cockroachdb/) | TiDB | Amazon Aurora | [MS Azure CosmosDB](azure-cosmos/) | [Google Cloud Spanner](google-spanner/) | YugaByte DB
--------|-----------------|------------|----------------|----------------|-------------|-----------
Horizontal write scalability | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check">| <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Auto-sharding and rebalancing   | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Automated Failover &amp; Repair  | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Distributed ACID transactions  | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
SQL Joins | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> |<i class="fas fa-times"></i>| <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Serializable isolation level | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Global consistency across multi-DC/regions | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-times"></i> | <i class="fas fa-times"> | <i class="fas fa-check"></i> |<i class="fas fa-check"></i>
Multiple read consistency levels | <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i>
Low, predictable p99 latencies | <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> 
SQL compatibility | PostgreSQL | MySQL | MySQL, PostgreSQL | Read Only | Proprietary | PostgreSQL (BETA)
Open Source | Apache 2.0 | Apache 2.0 | <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | Apache 2.0

## NoSQL databases

Feature  | [MongoDB](mongodb/) | [FoundationDB](foundationdb/) | [Apache Cassandra](cassandra/) |[Amazon DynamoDB](amazon-dynamodb/) | [MS Azure CosmosDB](azure-cosmos/)| YugaByte DB
--------|-----------|-------|--------|-------------|--------------|-----------------
Horizontal write scalability | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> |<i class="fas fa-check"></i>| <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Automated Failover &amp; Repair | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> |<i class="fas fa-check"></i>|<i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Auto-sharding and rebalancing | <i class="fas fa-check"></i> |<i class="fas fa-check"></i> |<i class="fas fa-check"></i>| <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Distributed ACID transactions  | <i class="fas fa-times"></i> |<i class="fas fa-check"></i> | <i class="fas fa-times"></i>| <i class="fas fa-check"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i>
Consensus-driven, strongly-consistent replication  | <i class="fas fa-times"></i> |<i class="fas fa-check"></i> | <i class="fas fa-times"></i>| <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i>
Strongly Consistent Secondary Indexes  | <i class="fas fa-times"></i> |<i class="fas fa-check"></i> | <i class="fas fa-times"></i>| <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i>
Multiple Read Consistency Levels | <i class="fas fa-check"></i> | <i class="fas fa-check"></i> |<i class="fas fa-check"></i>| <i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
Low, Predictable p99 Latencies | <i class="fas fa-times"></i> | <i class="fas fa-times"></i> |<i class="fas fa-times"></i>|<i class="fas fa-check"></i> | <i class="fas fa-check"></i> | <i class="fas fa-check"></i>
High data density| <i class="fas fa-times"></i> | <i class="fas fa-times"></i> |<i class="fas fa-times"></i>| <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | <i class="fas fa-check"></i>
API | MongoDB QL | Proprietary KV, MongoDB QL | Cassandra QL | Proprietary KV, Document | Cassandra QL, MongoDB QL | YCQL w/ Cassandra QL roots
Open Source | SSPL | Apache 2.0 | Apache 2.0 | <i class="fas fa-times"></i> | <i class="fas fa-times"></i> | Apache 2.0

{{< note title="Note" >}}
The <i class="fas fa-check"></i> or <i class="fas fa-times"></i> with respect to any particular feature of a third-party database is based on our best effort understanding from publicly available information. Readers are always recommended to perform their own independent research to understand the finer details.
{{< /note >}}
