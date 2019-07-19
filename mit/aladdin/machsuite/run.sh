#!/bin/bash

for d in */3/ ; do
   	echo $d
 	sh $d/run.sh
done 
