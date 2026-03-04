# The Prometheus Protocol

Seastar supports the Prometheus protocol for metrics reporting.
Supported exposition formats are the 0.0.4 text and protocol buffer formats.

More on the formats can be found at the [Prometheus documentations](https://prometheus.io/docs/instrumenting/exposition_formats/)

By default, Seastar would listen on port `9180` and the `localhost`.

See the Seastar configuration documentation on how to change the default configuration.

Seastar would reply based on the content type header, so pointing your browser to:
`http://localhost:9180/metrics/` will return a text representation of the metrics with their documentation.

Starting from Prometheus 2.0, the binary protocol is no longer supported.
While seastar still supports the binary protocol, it would be deprecated in a future release.

## Querying subset of the metrics
Seastar supports querying for a subset of the metrics by their names and labels.

### Filtering by a metric name
Use the `__name__` query parameter to select according to a metric name or a prefix.

For example, to get all the http metrics, point your browser to:
`http://localhost:9180/metrics?__name__=http*` note the asterisk symbol following the http.
Filtering by name only supports prefix matching.

To query for only the http requests served metric, point your browser to `http://localhost:9180/metrics?__name__=httpd_requests_served`

### Filtering by a label value
The Prometheus protocol uses labels to differentiate the characteristics of the thing that is being measured.
For example, in Seastar, it is common to report each metric per shard and add a `shard` label to the metric.

You can filter by any label using regular expressions. If you use multiple labels in your query, all conditions should be met.
A missing label is considered an empty string. The expression should match the entire label value,
to match a missing label, you can use `label=` or `label=^$`.

Here are a few examples:

To return all metrics from shard 1 or shard 0:
http://localhost:9180/metrics?shard=1|0

To get all metrics without a `service` label:
http://localhost:9180/metrics?service=

To get all metrics with a `service` label equals `prometheus` and from shard `0`:
http://localhost:9180/metrics?service=prometheus&shard=0

## Remove the help lines
Sending the help associated with each metric on each request is an overhead.
Prometheus itself does not use those help lines.
Seastar supports an option to remove those lines from the metrics output using the `__help__` query parameter.
To remove the help lines set `__help__=false`
for example:
`http://localhost:9180/metrics?__help__=false`

## Aggregation
In Seastar, metrics can be defined with implicit aggregation by specific labels,
which occurs at query time. This feature is useful, for instance to define metrics
per shard or even more finely grained per an application-defined entity while reporting
them in a more aggregated manner, such as sum or histogram per server.

However, there are times when it is necessary to inspect the fine-grained metrics.
This can be achieved by adding `__aggregate__=false` to the query string. For example:
`http://localhost:9180/metrics?__aggregate__=false`

### Configuring the Prometheus server for picking specific metrics
The [Prometheus configuration](https://prometheus.io/docs/prometheus/1.8/configuration/configuration/) describes the general Prometheus configuration.

To specify a specific metric or metrics add a `metrics_path` to the scrap config in the prometheue.yml file

For example, the following scrap config, will query for all the http metrics:

```
  scrape_configs:
    - job_name: http
      honor_labels: true
      metrics_path: /metrics
      params:
        __name__: ['http*']
```

