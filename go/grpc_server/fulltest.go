package grpc_server

import (
	"context"
	"encoding/hex"
	"fmt"
	"grpc_server/gen"
	"io"
	"log"
	"math"
	"net/http"
	"strings"
	"time"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/speedtest"
)

const (
	KiB = 1024
	MiB = 1024 * KiB
)

func getBetweenStr(str, start, end string) string {
	n := strings.Index(str, start)
	if n == -1 {
		return ""
	}
	str = str[n+len(start):]
	m := strings.Index(str, end)
	if m == -1 {
		return str
	}
	return str[:m]
}

func measureDownload(ctx context.Context, httpClient *http.Client, url string) string {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return "Error"
	}
	resp, err := httpClient.Do(req)
	if err != nil || resp == nil || resp.Body == nil {
		return "Error"
	}
	defer resp.Body.Close()

	timeStart := time.Now()
	n, copyErr := io.Copy(io.Discard, resp.Body)
	if copyErr != nil {
		return "Error"
	}
	duration := math.Max(time.Since(timeStart).Seconds(), 0.000001)
	return fmt.Sprintf("%.2fMiB/s", (float64(n)/duration)/MiB)
}

func DoFullTest(ctx context.Context, in *gen.TestReq, instance interface{}) (out *gen.TestResp, _ error) {
	out = &gen.TestResp{}
	httpClient := neko_common.CreateProxyHttpClient(instance)

	// Latency
	var latency string
	if in.FullLatency {
		t, _ := speedtest.UrlTest(httpClient, in.Url, in.Timeout, speedtest.UrlTestStandard_RTT)
		out.Ms = t
		if t > 0 {
			latency = fmt.Sprint(t, "ms")
		} else {
			latency = "Error"
		}
	}

	// UDP Latency
	var udpLatency string
	if in.FullUdpLatency {
		ctx, cancel := context.WithTimeout(ctx, time.Second*3)
		result := make(chan string, 1)

		go func() {
			var startTime = time.Now()
			pc, err := neko_common.DialContext(ctx, instance, "udp", "8.8.8.8:53")
			if err == nil {
				defer pc.Close()
				dnsPacket, _ := hex.DecodeString("0000010000010000000000000377777706676f6f676c6503636f6d0000010001")
				_, err = pc.Write(dnsPacket)
				if err == nil {
					var buf [1400]byte
					_, err = pc.Read(buf[:])
				}
			}
			if err == nil {
				var endTime = time.Now()
				result <- fmt.Sprint(endTime.Sub(startTime).Abs().Milliseconds(), "ms")
			} else {
				log.Println("UDP Latency test error:", err)
				result <- "Error"
			}
		}()

		select {
		case <-ctx.Done():
			udpLatency = "Timeout"
		case r := <-result:
			udpLatency = r
		}
		cancel()
	}

	// 入口 IP
	var in_ip string
	if in.FullInOut {
		// Resolving the profile server through net.ResolveIPAddr uses the
		// machine's system DNS and does not traverse the tested outbound.
		// Keep the routed egress-IP check below, but never perform this direct
		// ingress lookup in the product test path.
		in_ip = "Disabled (system DNS prohibited)"
	}

	// 出口 IP
	var out_ip string
	if in.FullInOut {
		req, requestErr := http.NewRequestWithContext(ctx, http.MethodGet, "https://www.cloudflare.com/cdn-cgi/trace", nil)
		if requestErr != nil {
			out_ip = "Error"
		} else {
			resp, err := httpClient.Do(req)
			if err == nil && resp != nil && resp.Body != nil {
				b, readErr := io.ReadAll(io.LimitReader(resp.Body, 64*KiB))
				_ = resp.Body.Close()
				out_ip = getBetweenStr(string(b), "ip=", "\n")
				if readErr != nil || out_ip == "" {
					out_ip = "Error"
				}
			} else {
				out_ip = "Error"
			}
		}
	}

	// 下载
	var speed string
	if in.FullSpeed {
		if in.FullSpeedTimeout <= 0 {
			in.FullSpeedTimeout = 30
		}

		ctx, cancel := context.WithTimeout(ctx, time.Second*time.Duration(in.FullSpeedTimeout))
		result := make(chan string, 1)

		go func() {
			result <- measureDownload(ctx, httpClient, in.FullSpeedUrl)
		}()

		select {
		case <-ctx.Done():
			speed = "Timeout"
		case s := <-result:
			speed = s
		}

		cancel()
	}

	fr := make([]string, 0)
	if latency != "" {
		fr = append(fr, fmt.Sprintf("Latency: %s", latency))
	}
	if udpLatency != "" {
		fr = append(fr, fmt.Sprintf("UDPLatency: %s", udpLatency))
	}
	if speed != "" {
		fr = append(fr, fmt.Sprintf("Speed: %s", speed))
	}
	if in_ip != "" {
		fr = append(fr, fmt.Sprintf("In: %s", in_ip))
	}
	if out_ip != "" {
		fr = append(fr, fmt.Sprintf("Out: %s", out_ip))
	}

	out.FullReport = strings.Join(fr, " / ")

	return
}
