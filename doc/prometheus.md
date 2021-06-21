# The Prometheus Protocol

Seastar supports the Prometheus protocol for metrics reporting.
Supported exposition formats are the 0.0.4 text and protocol buffer formats.

More on the formats can be found at the [Prometheus documentations](https://prometheus.io/docs/instrumenting/exposition_formats/)

By default, Seastar would listen on port `9180` and the `localhost`.

See the Seastar configuration documentation on how to change the default configuration.

Seastar would reply based on the content type header, so pointing your browser to:
`http://localhost:9180/metrics/` will return a text representation of the metrics with their documentation.

## Querying sub set of the metrics
Seastar supports specifying querying a specific metric or a metrics group that share the same prefix.

This is support by both protocols versions.

For example to get all the http metrics in a text format, point your browser to:
`http://localhost:9180/metrics?name=http*` note the asterisk symbol following the http. The Seastar Prometheus API only supports prefix matching.

To query for only the http requests served metric, point your browser to `http://localhost:9180/metrics?name=httpd_requests_served`

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
        name: 'http*'
``` 

