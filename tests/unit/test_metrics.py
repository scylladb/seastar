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

# Validate protobuf
for v in conf_metrics:
    res = query_prometheus(args.prometheus, v, conf_metrics[v]["type"])
    if res != conf_metrics[v]["value"]:
        print("Protobuf format: Metrics", v, "type", conf_metrics[v]["type"], "Mismatch, expected",
              res, "!=", conf_metrics[v]["value"])
