// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"net/http"
	"os"
	"strings"

	"github.com/golang/protobuf/jsonpb"
	sinkpb "go.chromium.org/luci/resultdb/sink/proto/v1"
	"go.fuchsia.dev/fuchsia/tools/lib/flagmisc"
	"golang.org/x/sync/errgroup"
)

var (
	summaries  flagmisc.StringsValue
	outputRoot string
)

func main() {
	if err := mainImpl(); err != nil {
		fmt.Fprintf(os.Stderr, "ResultDB upload errored: %v\n", err)
		os.Exit(1)
	}
}

func mainImpl() error {
	flag.Var(&summaries, "summary", "summary.json file location.")
	flag.StringVar(&outputRoot, "output", "",
		"Output root path to be joined with 'output_file' field in summary.json. If not set, current directory will be used.")

	flag.Parse()

	var requests []*sinkpb.ReportTestResultsRequest
	for _, summaryFile := range summaries {
		summary, err := ParseSummary(summaryFile)
		if err != nil {
			return err
		}
		testResults := SummaryToResultSink(summary, outputRoot)
		// Group 500 testResults per ReportTestResultsRequest. This reduces the number of HTTP calls
		// we make to result_sink. 500 is the maximum number of testResults allowed.
		requests = append(requests, createTestResultsRequests(testResults, 500)...)
	}

	invocationRequest := &sinkpb.ReportInvocationLevelArtifactsRequest{
		Artifacts: invocationLevelArtifacts(outputRoot),
	}

	client := &http.Client{}
	semaphore := make(chan struct{}, 64)
	for i := 0; i < cap(semaphore); i++ {
		semaphore <- struct{}{}
	}

	var eg errgroup.Group
	m := jsonpb.Marshaler{}
	ctx, err := resultSinkCtx()
	if err != nil {
		return err
	}

	for _, request := range requests {
		testResult, err := m.MarshalToString(request)
		if err != nil {
			return err
		}
		<-semaphore
		eg.Go(func() error {
			defer func() { semaphore <- struct{}{} }()
			return sendData(ctx, testResult, "ReportTestResults", client)
		})
	}

	testResult, err := m.MarshalToString(invocationRequest)
	if err != nil {
		return err
	}

	<-semaphore
	eg.Go(func() error {
		defer func() { semaphore <- struct{}{} }()
		return sendData(ctx, testResult, "ReportInvocationLevelArtifacts", client)
	})

	return eg.Wait()
}

func sendData(ctx *ResultSinkContext, data, endpoint string, client *http.Client) error {
	url := fmt.Sprintf("http://%s/prpc/luci.resultsink.v1.Sink/%s", ctx.ResultSinkAddr, endpoint)
	req, err := http.NewRequest("POST", url, strings.NewReader(data))
	if err != nil {
		return err
	}
	// ResultSink HTTP authorization scheme is documented at
	// https://fuchsia.googlesource.com/third_party/luci-go/+/HEAD/resultdb/sink/proto/v1/sink.proto#29
	req.Header.Add("Authorization", fmt.Sprintf("ResultSink %s", ctx.AuthToken))
	req.Header.Add("Accept", "application/json")
	req.Header.Add("Content-Type", "application/json")
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("ResultDB Http Request errored with status code %s (%d)", http.StatusText(resp.StatusCode), resp.StatusCode)
	}
	return nil
}

// createTestResultsRequests breaks an array of resultpb.TestResult into an array of resultpb.ReportTestResultsRequest
// chunkSize defined the number of TestResult contained in each ReportTrestResultsRequest.
func createTestResultsRequests(results []*sinkpb.TestResult, chunkSize int) []*sinkpb.ReportTestResultsRequest {
	totalChunks := (len(results)-1)/chunkSize + 1
	requests := make([]*sinkpb.ReportTestResultsRequest, totalChunks)
	for i := 0; i < totalChunks; i++ {
		requests[i] = &sinkpb.ReportTestResultsRequest{
			TestResults: make([]*sinkpb.TestResult, 0, chunkSize),
		}
	}
	for i, result := range results {
		requestIndex := i / chunkSize
		requests[requestIndex].TestResults = append(requests[requestIndex].TestResults, result)
	}
	return requests
}

// ResultSinkContext holds the result_sink information parsed from LUCI_CONTEXT
type ResultSinkContext struct {
	AuthToken      string `json:"auth_token"`
	ResultSinkAddr string `json:"address"`
}

func resultSinkCtx() (*ResultSinkContext, error) {
	v, ok := os.LookupEnv("LUCI_CONTEXT")
	if !ok {
		return nil, fmt.Errorf("LUCI_CONTEXT is not specified")
	}
	content, err := ioutil.ReadFile(v)
	if err != nil {
		return nil, err
	}
	var ctx struct {
		ResultSink ResultSinkContext `json:"result_sink"`
	}
	if err := json.Unmarshal(content, &ctx); err != nil {
		return nil, err
	}
	return &ctx.ResultSink, nil
}
