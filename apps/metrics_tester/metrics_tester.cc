/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2024 ScyllaDB
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/relabel_config.hh>
#include <seastar/core/internal/estimated_histogram.hh>
#include "../lib/stop_signal.hh"
#include <yaml-cpp/yaml.h>
using namespace seastar;
using namespace std::chrono_literals;
namespace sm = seastar::metrics;
struct serializer {};

struct metric_def {
    sstring name;
    sstring type;
    std::vector<double> values;
};

struct config {
    std::vector<metric_def> metrics;
};
namespace YAML {
template<>
struct convert<metric_def> {
    static bool decode(const Node& node, metric_def& cfg) {
        if (node["name"]) {
            cfg.name = node["name"].as<std::string>();
        }
        if (node["type"]) {
            cfg.type = node["type"].as<std::string>();
        }
        if (node["values"]) {
            cfg.values = node["values"].as<std::vector<double>>();
        }
        return true;
    }
};
template<>
struct convert<config> {
    static bool decode(const Node& node, config& cfg) {
        if (node["metrics"]) {
            cfg.metrics = node["metrics"].as<std::vector<metric_def>>();
        }
        return true;
    }
};
}
std::function<sm::internal::time_estimated_histogram()> make_histogram_fun(const metric_def& c) {
    sm::internal::time_estimated_histogram histogram;
    for (const auto& v : c.values) {
        histogram.add_micro(v);
    }
    return [histogram]() {return histogram;};
}

sm::impl::metric_definition_impl make_metrics_definition(const metric_def& jc, sm::label_instance private_label) {
    if (jc.type == "histogram") {
        sm::internal::time_estimated_histogram histogram;
        for (const auto& v : jc.values) {
            histogram.add_micro(v);
        }
        return sm::make_histogram(jc.name, [histogram]() {return histogram.to_metrics_histogram();},
                sm::description(jc.name),{private_label} );
    }
    if (jc.type == "gauge") {
        return sm::make_gauge(jc.name, [val=jc.values[0]] { return val; },
                sm::description(jc.name),{private_label} );
    }
    return sm::make_counter(jc.name, [val=jc.values[0]] { return val; },
            sm::description(jc.name),{private_label} );
}

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("listen", bpo::value<sstring>()->default_value("0.0.0.0"), "address to start Prometheus server on")
        ("port", bpo::value<uint16_t>()->default_value(9180), "Prometheus port")
        ("conf", bpo::value<sstring>()->default_value("./conf.yaml"), "config with jobs and options")
    ;
    httpd::http_server_control prometheus_server;

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            sm::metric_groups _metrics;
            seastar_apps_lib::stop_signal stop_signal;
            auto& opts = app.configuration();
            auto& listen = opts["listen"].as<sstring>();
            auto private_label = sm::label_instance("private", "1");
            auto& port = opts["port"].as<uint16_t>();
            auto& conf = opts["conf"].as<sstring>();

            YAML::Node doc = YAML::LoadFile(conf);
            auto cfg = doc.as<config>();

            for (auto&& jc : cfg.metrics) {
                _metrics.add_group("test_group", {
                        make_metrics_definition(jc, private_label)
                });
            }
            smp::invoke_on_all([] {
                    std::vector<metrics::relabel_config> rl(2);
                    rl[0].source_labels = {"__name__"};
                    rl[0].expr = ".*";
                    rl[0].action = metrics::relabel_config::relabel_action::drop;

                    rl[1].source_labels = {"private"};
                    rl[1].expr = "1";
                    rl[1].action = metrics::relabel_config::relabel_action::keep;
                    return metrics::set_relabel_configs(rl).then([](metrics::metric_relabeling_result) {
                        return;
                    });
            }).get();

            prometheus_server.start("prometheus").get();

            prometheus::config pctx;
            pctx.allow_protobuf = true;
            prometheus::start(prometheus_server, pctx).get();
            prometheus_server.listen(socket_address{listen, port}).handle_exception([] (auto ep) {
                return make_exception_future<>(ep);
            }).get();
            stop_signal.wait().get();
        });
    });
}
