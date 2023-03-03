
# gcp_file=results/gcp.csv
# offset_correction=-9
# egrep -o "Total average cycles spent:  ([0-9]+)" $gcp_file| awk 'BEGIN{ print "Offset,Latencies" } { print NR+'$offset_correction'","$5 }' > results/latencies

python3 plot.py  -z bar -l "ECS" --fontsize=25 --lloc=topleft  \
    -xc "Offset" -xl "Offset (Bytes)" \
    -yc "Latencies" -yl " " --ylim 100000 --ylog \
    -d results/latenciesAliECS -o results/membus_aliyun.pdf
# display results/membus_ali.pdf &
