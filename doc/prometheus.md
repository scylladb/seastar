# The Prometheus Protocol

Seastar supports the Prometheus protocol for metrics reporting.
Supported exposition formats are the 0.0.4 text and protocol buffer formats.

For more information about these formats, see the [Prometheus documentation](https://prometheus.io/docs/instrumenting/exposition_formats/).

By default, Seastar listens on port `9180` on `localhost`.

See the Seastar configuration documentation on how to change the default configuration.

Seastar uses the protocol buffer format only when it is enabled in the server
configuration and the request's `Accept` header contains a media type beginning with
`application/vnd.google.protobuf;`; otherwise, it uses the text format. Therefore,
opening `http://localhost:9180/metrics/` in a browser returns a text representation
of the metrics with their documentation.

Starting with Prometheus 2.0, the binary protocol is no longer supported.
While Seastar still supports the binary protocol, that support may be deprecated in a future release.

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
