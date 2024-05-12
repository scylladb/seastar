#!/usr/bin/env python3

import argparse
import requests
import yaml
import math
import re

MATCH_TYPE = re.compile("# TYPE (.*) (.*)")
MATCH_VALUE = re.compile(r".*\{.*\} ([\d]+)")
MATCH_HISTOGRAM = re.compile(r'.*\{.*le="([\d]+\.[\d]+)".*\} ([\d]+)')


def from_native_histogram(values):
    results = {}
    for v in values:
        results[val_to_bucket(float(v[2]) - 1)] = float(v[3])
    return results


def query_prometheus(host, query, type):
    url = "http://" + host + "/api/v1/query?query=" + query
    r = requests.get(url, headers={"Accept": "application/json"})
    results = r.json()["data"]["result"][0]
    return (
        from_native_histogram(results["histogram"][1]["buckets"])
        if type == "histogram"
        else float(results["value"][1])
    )


def validate_text(url):
    resp = requests.get("http://" + url)
    val = None
    res = {}
    for l in resp.iter_lines():
        if not l:
            continue
        ln = l.decode("utf-8")
        if "HELP" in ln:
            continue
        if "TYPE" in ln:
            if val:
                res[name] = {"name": name, "type": type, "value": val}
            m = MATCH_TYPE.match(ln)
            name = m.group(1)
            type = m.group(2)
            last_val = 0
            val = None
        else:
            if type == "histogram":
                m = MATCH_HISTOGRAM.match(ln)
                if not m:
                    continue
                le = val_to_bucket(float(m.group(1)) - 1)
                value = float(m.group(2))
                if not val:
                    val = {}
                if value > last_val:
                    val[le] = value - last_val
                    last_val = value
            else:
                m = MATCH_VALUE.match(ln)
                val = float(m.group(1))
    if val:
        res[name] = {"name": name, "type": type, "value": val}
    return res


def val_to_bucket(val):
    low = 2 ** math.floor(math.log(val, 2))
    high = 2 * low
    dif = (high - low) / 4
    return low + dif * math.floor((val - low) / dif)


def mk_histogram(values):
    hist = {}
    for val in values:
        bucket = val_to_bucket(val)
        if bucket not in hist:
            hist[bucket] = 1
        else:
            hist[bucket] = hist[bucket] + 1
    return hist


def conf_to_metrics(conf):
    res = {}
    for c in conf["metrics"]:
        name = "seastar_test_group_" + c["name"]
        res[name] = c
        res[name]["value"] = (
            mk_histogram(c["values"]) if c["type"] == "histogram" else c["values"][0]
        )
    return res


parser = argparse.ArgumentParser(
    description="Validate that the text and protobuf metrics representative work as expected. You will need to run metrics_tester and a Prometheus server that reads from the metrics_tester",
    conflict_handler="resolve",
)
parser.add_argument(
    "-h",
    "--host",
    default="localhost:9180/metrics",
    help="A host to connect to (the metrics_tester)",
)
parser.add_argument(
    "-p", "--prometheus", default="localhost:9090", help="A Prometheus to connect to"
)
parser.add_argument(
    "-c", "--config", default="conf.yaml", help="The metrics definition file"
)
args = parser.parse_args()

with open(args.config, "r") as file:
    metrics = yaml.safe_load(file)
    conf_metrics = conf_to_metrics(metrics)

from_text_metrics = validate_text(args.host)

# Validate text format
for v in conf_metrics:
    if v not in from_text_metrics:
        print("Text format: metrics ", v, "is missing")
    if from_text_metrics[v]["value"] != conf_metrics[v]["value"]:
        print('Text format: Metrics', v, 'type', from_text_metrics[v]['type'],
              'Mismatch, expected', from_text_metrics[v]['value'], '!=', conf_metrics[v]['value'])

# Validate protobuf
for v in conf_metrics:
    res = query_prometheus(args.prometheus, v, conf_metrics[v]["type"])
    if res != conf_metrics[v]["value"]:
        print("Protobuf format: Metrics", v, "type", conf_metrics[v]["type"], "Mismatch, expected",
              res, "!=", conf_metrics[v]["value"])
