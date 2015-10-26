#!/bin/bash
for filename in traces/*; do
  echo $filename
  ./mdriver -gvf $filename | tail -n 3
  echo
done
