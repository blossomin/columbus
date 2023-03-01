package main

import (
    "fmt"
	"net/http"
    "unsafe"
    "sync/atomic"
    "github.com/dterei/gotsc"
    "time"
)

import "C"

func Main(params map[string]interface{}) map[string]interface{} {

    cacheline_sz := uint64(C.sysconf(196))
    fmt.Printf("Cache line size: %d B\n", cacheline_sz)

    int_sz := unsafe.Sizeof(int(0))
    fmt.Printf("Array Integer size: %d B\n", int_sz)
    size := 10 * int(cacheline_sz) / int(int_sz)
    arr := make([]int, size)
    arr_start := unsafe.Pointer(&arr[0])

    // Get the cacheline boundary
    var i int
    for i = 0; i < size; i++ {
        addr := unsafe.Pointer(uintptr(arr_start) + uintptr(i)*unsafe.Sizeof(arr[0]))
        if uintptr(addr) % uintptr(cacheline_sz) == 0 {
            fmt.Println("The address at the beginning boundary of cache line is ", addr)
            break 
        }
    }

    if i == size {
        fmt.Println("Error! Could not find a cacheline boundary in the array")
    } else {
        var j int
        // Iterate over the memory region
        for j = 0; j < 15; j++ {
            // Address starting from cacheline - 12B
            addr := unsafe.Pointer(uintptr(arr_start) + unsafe.Sizeof(arr[0])*uintptr(i-2) + ((unsafe.Sizeof(arr[0]))/2)*uintptr(1) + uintptr(j))
            fmt.Println("Address: ", addr)
            st_time := time.Now()
            trials := uint64(0)
            total_cycles := uint64(0)

            // Measure CPU cycles for 3 sec
            for {
                tsc := gotsc.TSCOverhead()
                time.Sleep(10 * time.Millisecond)
                var k int
                for k = 1; k < 1000; k++ {
                    start := gotsc.BenchStart()
                    atomic.AddUintptr((*uintptr)(addr), uintptr(1))
                    end := gotsc.BenchEnd()

                    cycle_spent := end-start-tsc
                    trials++
                    total_cycles += cycle_spent
                } 

                if time.Since(st_time) >= 3*time.Second {
                    break
                }
            }
            fmt.Println("Total average cycles spent: ", total_cycles/trials)
        }
    }

    start := time.Now()

    /* 
    TODO: put your code here
    You can basically do anything you want,
    but please leave the return statement header
    and the success field as it is.
    */
	

    elapsed := time.Since(start)
    msg := map[string]interface{}{}
    msg["success"] = true
    msg["payload"] = map[string]interface{}{}
    msg["payload"].(map[string]interface{})["test"] = "cache latency test"
    msg["payload"].(map[string]interface{})["time"] = int64(elapsed / time.Millisecond)

    return msg
}
