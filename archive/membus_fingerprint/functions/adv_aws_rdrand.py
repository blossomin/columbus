import subprocess
import os
import subprocess
import random
import uuid
import json
import time
import psutil
import platform
from datetime import datetime
import rdrand

def get_size(bytes, suffix="B"):
    """
    Scale bytes to its proper format
    e.g:
        1253656 => '1.20MB'
        1253656678 => '1.17GB'
    """
    factor = 1024
    for unit in ["", "K", "M", "G", "T", "P"]:
        if bytes < factor:
            return f"{bytes:.2f}{unit}{suffix}"
        bytes /= factor

def lambda_handler(event, context):
    start = time.time()
    result = {}
    result["In"] = "Both"
    result['Start Time'] = int(round(time.time() * 1000))
    time.sleep(6)

    uname = platform.uname()
    result["Node Name"]=uname.node
    
    # CPU frequency
    cpufreq = psutil.cpu_freq()
    result["Current Frequency"]= cpufreq.current

    # MAC address
    if_addrs = psutil.net_if_addrs()
    for interface_name, interface_addresses in if_addrs.items():
        for address in interface_addresses:
            if str(address.family) == 'AddressFamily.AF_PACKET':
                result["MAC Address"] = address.address

    # get IO statistics since boot
    net_io = psutil.net_io_counters()
    result["Total Bytes Sent"] = get_size(net_io.bytes_sent)
    result["Total Bytes Received"] = get_size(net_io.bytes_recv)

    i = 8000 
    start1 = time.time()
    while(i!=0):
        rdrand.rdrand_get_bits(1000000)
        i = i-1
    end = time.time()
    result["Total Time"] = end-start
    result["Exec Time"] = end-start1

    return json.dumps(result)
