---
title: yb-docker-ctl
linkTitle: yb-docker-ctl
description: Use the "yb-docker-ctl" command line interface to administer local Docker clusters.
menu:
  latest:
    identifier: yb-docker-ctl
    parent: admin
    weight: 2420
aliases:
  - admin/yb-docker-ctl
isTocNested: false
showAsideToc: true
---

The `yb-docker-ctl` is a simple command line interface for administering local Docker clusters for development and learning. It manages the [`yb-master`](../yb-master/) and [`yb-tserver`](../yb-tserver/) containers to perform the necessary administration.

## Download

```sh
$ mkdir ~/yugabyte && cd ~/yugabyte
```

```sh
$ wget https://downloads.yugabyte.com/yb-docker-ctl && chmod +x yb-docker-ctl
```

## Help command

Use the **-\-help** option to see all of the supported commands.

```sh
$ ./yb-docker-ctl -h
```

```
usage: yb-docker-ctl [-h]
                     {create,add_node,status,destroy,stop_node,start_node,stop,start,remove_node}
                     ...

YugaByte Docker Container Control

positional arguments:
  {create,add_node,status,destroy,stop_node,start_node,stop,start,remove_node}
                        Commands
    create              Create YugaByte Cluster
    add_node            Add a new YugaByte Cluster Node
    status              Check YugaByte Cluster status
    destroy             Destroy YugaByte Cluster
    stop_node           Stop a YugaByte Cluster Node
    start_node          Start a YugaByte Cluster Node
    stop                Stop YugaByte Cluster so that it can be started later
    start               Start YugaByte Cluster if one already exists
    remove_node         Stop a YugaByte Cluster Node

optional arguments:
  -h, --help            show this help message and exit
```

## Create a cluster

Use the `yb-docker-ctl create` command to create a local YugaByte DB cluster with Docker containers. This cluster is intended for development and learning.

The number of nodes created with the initial create command is always equal to the replication factor in order to ensure that all the replicas for a given tablet can be placed on different nodes. With the [add_node](#add-a-node) and [remove_node](#remove-a-node) commands, the size of the cluster can thereafter be expanded or shrunk as needed.

### Create a 1 node local cluster with replication factor 1

Note that the create command pulls the latest `yugabytedb/yugabyte` image at the outset in case the image is not yet downloaded or is not the latest.

```sh
$ ./yb-docker-ctl create
```

### Create a 3 node local cluster with replication factor 3

Each of these initial nodes run a `yb-tserver` process and a `yb-master` process. Note that the number of yb-masters in a cluster has to equal to the replication factor for the cluster to be considered as operating normally and the number of yb-tservers is equal to be the number of nodes.

```sh
$ ./yb-docker-ctl create --rf 3
```

```
docker run --name yb-master-n1 --privileged -p 7000:7000 --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-master --fs_data_dirs=/mnt/disk0,/mnt/disk1 --master_addresses=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-master-n1:7100
Adding node yb-master-n1
docker run --name yb-master-n2 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-master --fs_data_dirs=/mnt/disk0,/mnt/disk1 --master_addresses=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-master-n2:7100
Adding node yb-master-n2
docker run --name yb-master-n3 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-master --fs_data_dirs=/mnt/disk0,/mnt/disk1 --master_addresses=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-master-n3:7100
Adding node yb-master-n3
docker run --name yb-tserver-n1 --privileged -p 9000:9000 -p 9042:9042 -p 6379:6379 --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-tserver-n1:9100
Adding node yb-tserver-n1
docker run --name yb-tserver-n2 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-tserver-n2:9100
Adding node yb-tserver-n2
docker run --name yb-tserver-n3 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-tserver-n3:9100
Adding node yb-tserver-n3
PID        Type       Node                 URL                       Status          Started At
11818      tserver    yb-tserver-n3        http://172.19.0.7:9000    Running         2017-11-28T23:33:00.369124907Z
11632      tserver    yb-tserver-n2        http://172.19.0.6:9000    Running         2017-11-28T23:32:59.874963849Z
11535      tserver    yb-tserver-n1        http://172.19.0.5:9000    Running         2017-11-28T23:32:59.444064946Z
11350      master     yb-master-n3         http://172.19.0.4:9000    Running         2017-11-28T23:32:58.899308826Z
11231      master     yb-master-n2         http://172.19.0.3:9000    Running         2017-11-28T23:32:58.403788411Z
11133      master     yb-master-n1         http://172.19.0.2:9000    Running         2017-11-28T23:32:57.905097927Z
```

#### Create a 5 node local cluster with replication factor 5

```sh
$ ./yb-docker-ctl create --rf 5
```

## Check cluster status

Get the status of your local cluster, including the URLs for the admin UIs for the YB-Master and YB-TServer.

```sh
$ ./yb-docker-ctl status
```

```
PID        Type       Node                 URL                       Status          Started At
11818      tserver    yb-tserver-n3        http://172.19.0.7:9000    Running         2017-11-28T23:33:00.369124907Z
11632      tserver    yb-tserver-n2        http://172.19.0.6:9000    Running         2017-11-28T23:32:59.874963849Z
11535      tserver    yb-tserver-n1        http://172.19.0.5:9000    Running         2017-11-28T23:32:59.444064946Z
11350      master     yb-master-n3         http://172.19.0.4:9000    Running         2017-11-28T23:32:58.899308826Z
11231      master     yb-master-n2         http://172.19.0.3:9000    Running         2017-11-28T23:32:58.403788411Z
11133      master     yb-master-n1         http://172.19.0.2:9000    Running         2017-11-28T23:32:57.905097927Z
```

## Add a node

Add a new node to the cluster. This will start a new yb-tserver process and give it a new `node_id` for tracking purposes.

```sh
$ ./yb-docker-ctl add_node
```

```
docker run --name yb-tserver-n4 --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=04:7100,04:7100,04:7100 --rpc_bind_addresses=yb-tserver-n4:9100
Adding node yb-tserver-n4
```

## Remove a node

Remove a node from the cluster by executing the following command. The command takes the node_id of the node to be removed as input.

### Help

```sh
$ ./yb-docker-ctl remove_node --help
```

```
usage: yb-docker-ctl remove_node [-h] node

positional arguments:
  node_id        Index of the node to remove

optional arguments:
  -h, --help  show this help message and exit
```

### Example

```sh
$ ./yb-docker-ctl remove_node 3
```

```
Stopping node :yb-tserver-n3
```

## Destroy cluster

The `yb-docker-ctl destroy` command below destroys the local cluster, including deletion of the data directories.

```sh
$ ./yb-docker-ctl destroy
```

## Upgrade container image

The following `docker pull` command below upgrades the Docker image of YugaByte DB to the latest version.

```sh
$ docker pull yugabytedb/yugabyte
```
