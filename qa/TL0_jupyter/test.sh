#!/bin/bash -e
# used pip packages
pip_packages="jupyter numpy matplotlib pillow"
target_dir=./docs/examples

do_once() {
    # attempt to run jupyter on all example notebooks
    mkdir -p idx_files
}

test_body() {
    # test code
    # dummy patern
    black_list_files="multigpu"

    ls *.ipynb | sed "/${black_list_files}/d" | xargs -i jupyter nbconvert \
                    --to notebook --inplace --execute \
                    --ExecutePreprocessor.kernel_name=python${PYVER:0:1} \
                    --ExecutePreprocessor.timeout=300 {}
}

pushd ../..
source ./qa/test_template.sh
popd
