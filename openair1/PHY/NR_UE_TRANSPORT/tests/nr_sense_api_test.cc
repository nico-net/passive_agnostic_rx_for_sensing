#include <gtest/gtest.h>
#include "nr_sense_api.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>
#ifdef NR_SENSE_HAVE_ZMQ
#include <zmq.h>
#endif

namespace {
std::string temp_path() {
  char path[] = "/tmp/sense_api_XXXXXX";
  int fd = mkstemp(path);
  if (fd >= 0) close(fd);
  return path;
}
std::vector<uint8_t> bytes(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
uint16_t u16(const std::vector<uint8_t> &b, size_t p) { return b[p] | (uint16_t(b[p+1]) << 8); }
uint32_t u32(const std::vector<uint8_t> &b, size_t p) { return u16(b,p) | (uint32_t(u16(b,p+2)) << 16); }
uint64_t u64(const std::vector<uint8_t> &b, size_t p) { return u32(b,p) | (uint64_t(u32(b,p+4)) << 32); }
float f32(const std::vector<uint8_t> &b, size_t p) { uint32_t x=u32(b,p); float f; memcpy(&f,&x,4); return f; }
struct SenseApi : ::testing::Test {
  std::string rec=temp_path(), cfr=temp_path();
  nr_sense_cfg_t cfg{};
  void SetUp() override { cfg={rec.c_str(),cfr.c_str(),nullptr,16,64,16}; }
  void TearDown() override { nr_sense_test_pause_writer(false); nr_sense_test_fail_writes(false); nr_sense_close(); unlink(rec.c_str()); unlink(cfr.c_str()); }
};
}

TEST_F(SenseApi, RoundTripCfrFrame) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2*12*2]; uint16_t k[12]; uint8_t l[12];
  for(int i=0;i<48;i++) iq[i]=i+0.25f;
  for(int i=0;i<12;i++) { k[i]=100+i; l[i]=i%4; }
  bool truncated=true;
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_CSIRS,2,iq,12,12,k,l,1234,0.5f,2.25f,7,9,&truncated),1u);
  EXPECT_FALSE(truncated);
  nr_sense_close(); auto b=bytes(cfr);
  ASSERT_EQ(b.size(),48u+36u+192u);
  EXPECT_EQ(std::string(b.begin(),b.begin()+4),"NRCF");
  EXPECT_EQ(u16(b,4),1); EXPECT_EQ(u16(b,6),48); EXPECT_EQ(u64(b,8),1u);
  EXPECT_EQ(b[16],NR_SENSE_SRC_CSIRS); EXPECT_EQ(b[17],2); EXPECT_EQ(u16(b,18),0);
  EXPECT_EQ(u32(b,20),12u); EXPECT_EQ(u64(b,24),1234u);
  EXPECT_FLOAT_EQ(f32(b,32),0.5f); EXPECT_FLOAT_EQ(f32(b,36),2.25f);
  EXPECT_EQ(u32(b,40),7u); EXPECT_EQ(u32(b,44),9u);
  for(int i=0;i<12;i++) { EXPECT_EQ(u16(b,48+2*i),k[i]); EXPECT_EQ(b[72+i],l[i]); }
  for(int i=0;i<48;i++) EXPECT_FLOAT_EQ(f32(b,84+4*i),iq[i]);
}

TEST_F(SenseApi, RecordsAreJsonLines) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  for(int i=0;i<3;i++) EXPECT_TRUE(nr_sense_push_record("{\"schema\":2,\"type\":\"rs\"}"));
  nr_sense_close(); std::ifstream f(rec); std::string line; int count=0;
  while(std::getline(f,line)) { EXPECT_EQ(line,"{\"schema\":2,\"type\":\"rs\"}"); count++; }
  EXPECT_EQ(count,3);
}

TEST_F(SenseApi, SlowWriterNeverBlocksProducer) {
  nr_sense_test_pause_writer(true); ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={1,2}; uint16_t k=3; uint8_t l=4;
  auto start=std::chrono::steady_clock::now();
  for(int i=0;i<10000;i++) nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr);
  auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_LT(ms,50); EXPECT_EQ(s.cfr_pushed,16u); EXPECT_EQ(s.cfr_dropped[NR_SENSE_SRC_SSB],9984u);
}

TEST_F(SenseApi, IoErrorCounted) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  nr_sense_test_fail_writes(true);
  EXPECT_TRUE(nr_sense_push_record("{\"schema\":2}"));
  nr_sense_close(); nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_GT(s.io_errors,0u);
}

TEST_F(SenseApi, OversizeFrameTruncatedAndFlagged) {
  cfg.cfr_max_re=4; ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[18]; uint16_t k[9]; uint8_t l[9];
  for(int i=0;i<18;i++) iq[i]=i;
  for(int i=0;i<9;i++) { k[i]=i; l[i]=i; }
  bool truncated=false;
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,9,9,k,l,0,0,0,0,0,&truncated),1u);
  nr_sense_close(); auto b=bytes(cfr); nr_sense_stats_t s{}; nr_sense_stats(&s);
  ASSERT_GE(b.size(),48u); EXPECT_EQ(u32(b,20),4u); EXPECT_TRUE(truncated); EXPECT_EQ(s.truncated,1u);
}

TEST_F(SenseApi, CloseTwiceAndPushAfterCloseSafe) {
  ASSERT_EQ(nr_sense_open(&cfg),0); nr_sense_close(); nr_sense_close();
  EXPECT_FALSE(nr_sense_enabled()); EXPECT_FALSE(nr_sense_push_record("{\"schema\":2}"));
  float iq[2]={}; uint16_t k=0; uint8_t l=0;
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr),0u);
  nr_sense_stats_t s{}; nr_sense_stats(&s); EXPECT_EQ(s.after_close,2u);
}

TEST_F(SenseApi, ZmqMirrorOptional) {
#ifndef NR_SENSE_HAVE_ZMQ
  GTEST_SKIP() << "libzmq not configured";
#else
  std::string ep="inproc://sense_api_test";
  cfg.endpoint=ep.c_str(); ASSERT_EQ(nr_sense_open(&cfg),0);
  void *sub=zmq_socket(nr_sense_test_zmq_context(),ZMQ_SUB); ASSERT_NE(sub,nullptr);
  int timeout=2000; zmq_setsockopt(sub,ZMQ_RCVTIMEO,&timeout,sizeof(timeout));
  ASSERT_EQ(zmq_setsockopt(sub,ZMQ_SUBSCRIBE,"cfr",3),0);
  ASSERT_EQ(zmq_connect(sub,ep.c_str()),0);
  usleep(150000); // PUB/SUB subscription propagation
  float iq[2]={1,2}; uint16_t k=3; uint8_t l=4;
  for(int i=0;i<4;i++) { nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr); usleep(20000); }
  char topic[16]; ASSERT_EQ(zmq_recv(sub,topic,sizeof(topic),0),3); EXPECT_EQ(std::string(topic,3),"cfr");
  std::vector<uint8_t> payload(128); int n=zmq_recv(sub,payload.data(),payload.size(),0); ASSERT_GT(n,0);
  zmq_close(sub);
  nr_sense_close(); auto file=bytes(cfr); ASSERT_GE(file.size(),size_t(n));
  EXPECT_TRUE(std::equal(payload.begin(),payload.begin()+n,file.begin()));
#endif
}
