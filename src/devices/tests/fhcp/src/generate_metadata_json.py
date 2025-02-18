# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
Parses the intermediate JSON provided and generates a new JSON containing
metadata about FHCP tests.
"""
import argparse
import json
import os
import sys


def validate(data):
    tests = data["tests"]
    test_types = data["driver_test_types"]
    categories = data["device_category_types"]
    for test in tests:
        url = test["url"]
        if not test["test_types"]:
            raise ValueError(
                f"The test {url} must specify at least one category.")
        for item in test["test_types"]:
            if item not in test_types:
                raise ValueError(
                    f"The test {url} specifies an invalid category '{item}'.")
        if not test["device_categories"]:
            raise ValueError(f"The test {url} must specify at least one type.")
        for item in test["device_categories"]:
            item_type = item["category"]
            if item_type not in categories:
                raise ValueError(
                    f"The test {url} specifies an invalid type '{item_type}'.")
            item_sub_type = item["subcategory"]
            if item_sub_type and item_sub_type not in categories[item_type]:
                raise ValueError(
                    f"The test {url} specifies an invalid sub-type '{item_sub_type}'."
                )
    return True


def convert_to_final_dict(appendix, data):
    test_entries = {}
    fhcp_entries = {}

    # The `data` is a mix of two different sources of JSON.
    # Type A is generated by `fuchsia_test_package` under the "tests"
    # metadata field.
    # Type B is generated by `fhcp_test_package` under the "fhcp"
    # metadata field.
    for d in data:
        if "test" in d and "build_rule" in d["test"]:
            # Found a "Type A" as mentioned above.
            test_entries[d["test"]["package_label"]] = d
        elif "test_types" in d and "device_categories" in d:
            # Found a "Type B" as described above.
            fhcp_entries[d["id"]] = d
        else:
            raise ValueError("This is not a valid entry:", d)
    tests = []
    for entry in fhcp_entries:
        if entry not in test_entries:
            raise ValueError(f"Did not find '{entry}' in the tests.")
        test_metadata = test_entries[entry]
        fhcp_metadata = fhcp_entries[entry]
        tests.append(
            {
                "url": test_metadata["test"]["package_url"],
                "test_types": fhcp_metadata["test_types"],
                "device_categories": fhcp_metadata["device_categories"],
                "environments": fhcp_metadata["environments"],
            })
    appendix["tests"] = tests
    return appendix


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--appendix_json', help="Path to the appendix JSON", required=True)
    parser.add_argument(
        '--intermediate_json', help="Path to intermediate JSON", required=True)
    parser.add_argument(
        '--output_json', help="Path that we will output to.", required=True)
    args = parser.parse_args()

    appendix = {}
    with open(args.appendix_json, "r") as f:
        appendix = json.load(f)
    intermediate = {}
    with open(args.intermediate_json, "r") as f:
        intermediate = json.load(f)

    final_dict = convert_to_final_dict(appendix, intermediate)
    validate(final_dict)

    with open(args.output_json, 'w') as output:
        output.write(str(json.dumps(final_dict)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
