# The Prometheus Protocol

Seastar supports the Prometheus protocol for metrics reporting.

For more information about the formats, see the [Prometheus documentation](https://prometheus.io/docs/instrumenting/exposition_formats/).

By default, Seastar listens on port `9180` on `localhost`.

See the Seastar configuration documentation on how to change the default configuration.

Seastar chooses the exposition format from the request's `Accept` header (see
[Exposition formats](#exposition-formats)) and uses the 0.0.4 text format by default.
Therefore, opening `http://localhost:9180/metrics/` in a browser returns a text
representation of the metrics with their documentation.

## Exposition formats
Seastar selects the exposition format from the `Accept` header of the request.
The following formats are supported:

| Format | `Accept` media type | Default |
|--------|---------------------|---------|
| Prometheus text format 0.0.4 | `text/plain; version=0.0.4` (or anything else) | yes |
| Prometheus protocol buffers | `application/vnd.google.protobuf; proto=io.prometheus.client.MetricFamily; encoding=delimited` | no, requires `config::allow_protobuf` |
| OpenMetrics 2.0 text format | `application/openmetrics-text; version=2.0.0` | no, selected by the `Accept` header |

When the header asks for both protobuf and OpenMetrics 2.0, the one with the higher quality (`q`) is used,
and protobuf on a tie. OpenMetrics 2.0 is only used when its quality is higher than that of the 0.0.4 text format
(`text/plain`). An entry with `q=0` is refused.

### Prometheus text format 0.0.4
This is the default format, it is used when the `Accept` header is missing or asks for any other format.
Histograms and summaries are reported as multiple series: a histogram as `_sum`, `_count` and `_bucket` with an `le`
label, a summary as `_sum`, `_count` and the metric name with a `quantile` label.

### Protocol buffers
The protocol buffers format is the only format in which Prometheus versions before 3.0 can scrape
[native histograms](https://prometheus.io/docs/specs/native_histograms/).
It is experimental and disabled by default. To enable it, set `allow_protobuf` in the `seastar::prometheus::config`
that the Prometheus server is started with.
When enabled, Seastar reports a histogram as a native histogram whenever the histogram
implementation provides the native histogram information (as `approximate_exponential_histogram` does).

Starting from Prometheus 2.0, the binary protocol was no longer supported as a scrape format,
Prometheus re-introduced it for native histograms.

### OpenMetrics 2.0 text format
[OpenMetrics 2.0](https://prometheus.io/docs/specs/om/open_metrics_spec_2_0/) is a new text format. It is closer
to the 0.0.4 text format than to OpenMetrics 1.0, and it supports native histograms. The
[migration guide](https://prometheus.io/docs/guides/open_metrics_2_0_migration/) describes how it differs from the
earlier formats. Its support is experimental. A request with the
`Accept: application/openmetrics-text; version=2.0.0` header is answered with a
`Content-Type: application/openmetrics-text; version=2.0.0; charset=utf-8` response, for example:

```
curl -H 'Accept: application/openmetrics-text; version=2.0.0' http://localhost:9180/metrics
```

Compared to the 0.0.4 format:
- Histograms and summaries are reported as a single composite value. A histogram with fixed buckets is
  reported as `{count:C,sum:S,bucket:[le:count,...,+Inf:C]}`, a summary as `{count:C,sum:S,quantile:[q:v,...]}`.
- A histogram that provides native histogram information is reported as a native histogram:
  `{count:C,sum:S,schema:N,zero_threshold:0,zero_count:0,positive_spans:[offset:length,...],positive_buckets:[count,...]}`.
- A metric can declare its unit with the internal `__unit` label.
  The label itself is never reported. Instead, a `# UNIT` metadata line is added.
- Metric and label names are reported unchanged, like in the 0.0.4 format. OpenMetrics 2.0 only
  recommends (and does not require) the `_total` suffix for counters and the unit as a name suffix,
  so Seastar does not rename metrics. Applications that want to follow the recommendation can name
  their metrics accordingly (e.g. `read_bytes_total` with unit `bytes`). Metric names are expected to use
  the Prometheus character set (`[a-zA-Z_:][a-zA-Z0-9_:]*`). A label name outside `[a-zA-Z_][a-zA-Z0-9_]*`
  is written in double quotes, as OpenMetrics 2.0 requires.
- Exemplars of histograms (`seastar::metrics::histogram::exemplars`, filled by the metric callback) are written
  after the composite value in timestamp order, as `# {trace_id="..."} value timestamp`. The timestamp is in
  seconds since the epoch. OpenMetrics 2.0 defines exemplars for histograms and counters only, and Prometheus drops
  the exemplars of a summary, so they are not written for summaries. Prometheus also drops an exemplar whose label
  names and values are longer than 128 UTF-8 characters together, so keep the labels short, for example a trace id
  alone. The 0.0.4 text format and protocol buffers do not report exemplars.
- The exposition ends with a `# EOF` line.

For example:
```
# TYPE seastar_httpd_read_bytes counter
# UNIT seastar_httpd_read_bytes bytes
# HELP seastar_httpd_read_bytes The total number of bytes read
seastar_httpd_read_bytes{shard="0"} 1024
# TYPE seastar_reactor_io_latency histogram
# HELP seastar_reactor_io_latency IO latency
seastar_reactor_io_latency{shard="0"} {count:59,sum:120,schema:3,zero_threshold:0,zero_count:0,positive_spans:[0:3,2:2],positive_buckets:[10,15,12,8,12]}
# EOF
```

## Querying a subset of the metrics
Seastar supports querying for a subset of the metrics by their names and labels.

Filtering is recommended when you only need a subset of the available metrics, especially
in systems with many metrics. Filtering reduces the processing overhead on the Seastar
application, results in smaller HTTP responses, and decreases the load on your Prometheus
server when scraping and storing metrics.

### Filtering by a metric name
Use the `__name__` query parameter to select according to a metric name or a prefix.

For example, to get all HTTP metrics, open
`http://localhost:9180/metrics?__name__=http*`. Note the asterisk following `http`.
Filtering by name only supports prefix matching.

To query for only the HTTP requests-served metric, open `http://localhost:9180/metrics?__name__=httpd_requests_served`.

You can use either the full metric name as it appears in the output (e.g., `seastar_httpd_requests_served`)
or the name without the prefix (e.g., `httpd_requests_served`). Both forms work identically.

#### Multiple name filters
You can specify multiple `__name__` parameters to query for several specific metrics at once.
A metric is included if it matches any of the specified names.

For example, to get both the HTTP requests and connections metrics:
`http://localhost:9180/metrics?__name__=httpd_requests_served&__name__=httpd_connections_total`

This also works with prefix matching:
`http://localhost:9180/metrics?__name__=httpd_requests*&__name__=httpd_connections*`

### Filtering by a label value
The Prometheus protocol uses labels to differentiate the characteristics of the thing that is being measured.
For example, in Seastar, it is common to report each metric per shard and add a `shard` label to the metric.

You can filter by any label using regular expressions. If you use multiple labels in your query, all conditions must be met.
A missing label is considered an empty string. The expression should match the entire label value;
to match a missing label, you can use `label=` or `label=^$`.

Here are a few examples:

To return all metrics from shard 1 or shard 0:
http://localhost:9180/metrics?shard=1|0

To get all metrics without a `service` label:
http://localhost:9180/metrics?service=

To get all metrics whose `service` label equals `prometheus` and that come from shard `0`:
http://localhost:9180/metrics?service=prometheus&shard=0

## Remove the help lines
Sending the help associated with each metric on every request adds overhead.
Prometheus itself does not use those help lines.
Seastar supports an option to remove those lines from the metrics output using the `__help__` query parameter.
To remove the help lines, set `__help__=false`. For example:
`http://localhost:9180/metrics?__help__=false`

## Aggregation
In Seastar, metrics can be defined with implicit aggregation by specific labels,
which occurs at query time. This feature is useful, for instance to define metrics
per shard or even more finely grained per an application-defined entity while reporting
them in a more aggregated manner, such as sum or histogram per server.

However, there are times when it is necessary to inspect the fine-grained metrics.
This can be achieved by adding `__aggregate__=false` to the query string. For example:
`http://localhost:9180/metrics?__aggregate__=false`

### Configuring the Prometheus server to select specific metrics
The [Prometheus configuration documentation](https://prometheus.io/docs/prometheus/latest/configuration/configuration/) describes the general configuration format.

To specify one or more metrics, add a `metrics_path` to the scrape configuration in the `prometheus.yml` file.

For example, the following scrape configuration queries all HTTP metrics:

```
  scrape_configs:
    - job_name: http
      honor_labels: true
      metrics_path: /metrics
      params:
        __name__: ['http*']
```

To query for multiple specific metrics, list them in the array:

```
  scrape_configs:
    - job_name: selected_metrics
      honor_labels: true
      metrics_path: /metrics
      params:
        __name__: ['httpd_requests_served', 'httpd_connections_total', 'scheduler*']
```
