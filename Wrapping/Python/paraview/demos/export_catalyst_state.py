r"""
This script takes in any PVSM file and generates a Catalyst Python state file.
"""

import argparse

parser = argparse.ArgumentParser(\
    description="Generate Catalyst Python State")
parser.add_argument("--output", type=str,
    help="name for the output file (*.zip)", required=True)
parser.add_argument("--extracts-dir", type=str,
    help="path to directory where to make the Catalyst script save generated extracts",
    required=True)
parser.add_argument("--pvsm", type=str,
    help="pvsm state file to load", required=True)

args = parser.parse_args()

from paraview import simple, catalyst
simple.LoadState(args.pvsm)

options = catalyst.Options()
options.DataExtractsOutputDirectory = args.extracts_dir
options.ImageExtractsOutputDirectory = args.extracts_dir

# TODO: add a 'simple' version for this, we don't want users to
# import anything from 'detail' and hence we don't want that in a demo
# script.
import os, os.path
# remove output if it exists
if os.path.exists(args.output):
    print("removing existing output file:", args.output)
    os.remove(args.output)

from paraview.detail.catalyst_export import save_catalyst_package_as_zip
save_catalyst_package_as_zip(args.output, options)

assert os.path.exists(args.output)
