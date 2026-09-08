#include <gtest/gtest.h>
extern "C" int offline_cfo(void);
extern "C" int offline_sfo(void);
extern "C" int offline_feedback(void);
extern "C" int offline_joint(void);
TEST(OfflineSync, RealFepCfoSign) { ASSERT_EQ(offline_cfo(),0); }
TEST(OfflineSync, RealEqualizerSfoOriginAndSign) { ASSERT_EQ(offline_sfo(),0); }
TEST(OfflineSync, RealFepFeedbackLaw) { ASSERT_EQ(offline_feedback(),0); }
TEST(OfflineSync, JointCfoSfoSeparation) { ASSERT_EQ(offline_joint(),0); }

extern "C" int offline_snapshot(void);
TEST(OfflineSync, RealFepDeferredCfoSnapshotSurvivesWorkerDispatch) { ASSERT_EQ(offline_snapshot(),0); }
