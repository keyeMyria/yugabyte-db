---
title: MONEY data types
linktitle: Money
summary: MONEY data types
description: MONEY data types
menu:
  latest:
    identifier: api-ysql-datatypes-money
    parent: api-ysql-datatypes
aliases:
  - /latest/api/ysql/datatypes/type_money
isTocNested: true
showAsideToc: true
---

## Synopsis

MONEY data type represents currency with a fixed precision for fraction.

Data type | Description | Min | Max |
----------|-------------|-----|-----|
MONEY | 8 bytes | -92233720368547758.08 | +92233720368547758.07 |

## Description

```
type_specification ::= MONEY
```

- To avoid precision loss, MONEY value can be cast to NUMERIC type before applying calculations.
