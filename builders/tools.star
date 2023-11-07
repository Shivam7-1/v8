# Copyright 2021 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

load("//lib/lib.star", "RECLIENT", "defaults_ci", "v8_builder")
load(
    "//lib/service-accounts.star",
    "V8_CI_ACCOUNT",
    "V8_TEST262_IMPEXP_ACCOUNT",
)

v8_builder(
    defaults_ci,
    name = "Infra Expriments",
    bucket = "ci",
    dimensions = {"os": "Ubuntu-22.04", "cpu": "x86-64"},
    service_account = V8_CI_ACCOUNT,
    executable = "recipe:v8/spike",
    schedule = "* * * * * 1970",
    in_list = "tools",
)

v8_builder(
    defaults_ci,
    name = "Branch Monitor",
    bucket = "ci",
    dimensions = {"os": "Ubuntu-22.04", "cpu": "x86-64"},
    service_account = V8_CI_ACCOUNT,
    executable = "recipe:v8/branch_monitor",
    properties = {"max_gap_seconds": 43200},
    schedule = "49 * * * *",
    in_list = "tools",
    notifies = ["branch monitor", "infra"],
)

v8_builder(
    name = "Test262 exporter",
    bucket = "ci-hp",
    dimensions = {"os": "Ubuntu-22.04", "cpu": "x86-64"},
    service_account = V8_TEST262_IMPEXP_ACCOUNT,
    executable = "recipe:v8/test262_export",
    schedule = "* * * * * 1970",
    in_list = "tools",
    execution_timeout = 3600,
    notifies = ["test262 impex", "infra"],
)

v8_builder(
    name = "Test262 importer",
    bucket = "ci-hp",
    dimensions = {"os": "Ubuntu-22.04", "cpu": "x86-64"},
    service_account = V8_TEST262_IMPEXP_ACCOUNT,
    executable = "recipe:v8/test262_import",
    schedule = "* * * * * 1970",
    in_list = "tools",
    execution_timeout = 3600,
    notifies = ["test262 impex", "infra"],
)

v8_builder(
    name = "Test262 import watcher",
    bucket = "ci-hp",
    dimensions = {"os": "Ubuntu-22.04", "cpu": "x86-64"},
    service_account = V8_TEST262_IMPEXP_ACCOUNT,
    executable = "recipe:v8/roll_watch",
    schedule = "* * * * * 1970",
    in_list = "tools",
    execution_timeout = 3600,
    notifies = ["test262 impex", "infra"],
    properties = {
        "watched_rollers": [{
            "name": "Test262 import watcher",
            "subject": "[test262] Roll test262",
            "review-host": "chromium-review.googlesource.com",
            "project": "v8/v8",
            "account": V8_TEST262_IMPEXP_ACCOUNT,
        }],
    },
)
