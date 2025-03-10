---
title: Auto-sharding
linkTitle: 4. Auto-sharding
description: Auto-sharding
aliases:
  - /explore/auto-sharding/
  - /latest/explore/auto-sharding/
  - /latest/explore/planet-scale/auto-sharding/
  - /explore/auto-rebalancing/
  - /latest/explore/auto-rebalancing/
  - /latest/explore/planet-scale/auto-rebalancing/
menu:
  latest:
    identifier: auto-sharding
    parent: explore
    weight: 225
---

YugaByte DB automatically splits user tables into multiple shards, called **tablets**. The primary key for each row in the table uniquely determines the tablet the row lives in. For data distribution purposes, a hash based partitioning scheme is used. Read more about [how sharding works](../../architecture/concepts/docdb/sharding/) in YugaByte DB.

By default, YugaByte creates 8 tablets per node in the cluster for each table and automatically distributes the data across the various tablets, which in turn are distributed evenly across the nodes. In this tutorial, we will explore how automatic sharding is done internally for tables. The system Redis table works in an identical manner.

We will explore automatic sharding inside YugaByte DB by creating these tables:

- Use a replication factor of 1. This will make it easier to understand how automatic sharding is achieved independent of data replication.
- Insert entries one by one, and examine which how the data gets distributed across the various nodes.

If you haven't installed YugaByte DB yet, do so first by following the [Quick Start](../../quick-start/install/) guide.

<ul class="nav nav-tabs nav-tabs-yb">
  <li>
    <a href="#macos" class="nav-link active" id="macos-tab" data-toggle="tab" role="tab" aria-controls="macos" aria-selected="true">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>
  <li>
    <a href="#linux" class="nav-link" id="linux-tab" data-toggle="tab" role="tab" aria-controls="linux" aria-selected="false">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="macos" class="tab-pane fade show active" role="tabpanel" aria-labelledby="macos-tab">
    {{% includeMarkdown "binary/auto-sharding.md" /%}}
  </div>
  <div id="linux" class="tab-pane fade" role="tabpanel" aria-labelledby="linux-tab">
    {{% includeMarkdown "binary/auto-sharding.md" /%}}
  </div>
  <!--
  <div id="docker" class="tab-pane fade" role="tabpanel" aria-labelledby="docker-tab">
    {{% includeMarkdown "docker/auto-sharding.md" /%}}
  </div>
  <div id="kubernetes" class="tab-pane fade" role="tabpanel" aria-labelledby="kubernetes-tab">
    {{% includeMarkdown "kubernetes/auto-sharding.md" /%}}
  </div>
  -->
</div>
