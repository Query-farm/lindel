<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# Lindel (linearizer-delinearizer) Extension for DuckDB

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/lindel.html)
[![v1.5 build](https://github.com/Query-farm/lindel/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/lindel/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

This `lindel` extension adds functions for the [linearization](https://en.wikipedia.org/wiki/Linearization) and delinearization of numeric arrays in [DuckDB](https://www.duckdb.org). It allows you to order multi-dimensional data using space-filling curves.

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/lindel](https://query.farm/products/extensions/lindel)**

## Installation

```sql
INSTALL lindel FROM community;
LOAD lindel;
```

## Development

For instructions on building the extension from source and running its tests, see [BUILDING.md](BUILDING.md).

## Compatibility notes

Version `2026100701` fixes signed one-dimensional Hilbert/Morton decoding and four-dimensional 8-bit integer decoding. Encoded keys remain unsigned; `return_unsigned = false` selects signed coordinates. Decoding a `UINTEGER` key as floating-point now rejects dimension counts other than one instead of silently ignoring the requested count.

Older community binaries may retain the previous behavior until the fixed source is published.
