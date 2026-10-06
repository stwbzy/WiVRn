#include "home_view.h"
#include "render_tuning.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace wivrn::launcher;
using mocktail::vr::ScriptedPoseSample;

namespace
{
ScriptedPoseSample Head(bool held = true)
{
	ScriptedPoseSample sample;
	sample.valid = true;
	sample.frame = 1;
	sample.controllers_connected = true;
	sample.hands[0].grip_valid = true;
	sample.hands[0].aim_valid = true;
	sample.position[0] = .7f;
	sample.position[1] = -.95f;
	sample.position[2] = -.2f;
	sample.controller_channels[27] = held ? 1.f : 0.f;
	return sample;
}

TEST(HomeView, ShortPressNeverRecenters)
{
	HomeView view;
	view.Update(Head(), true, 1, 1);
	view.Update(Head(), true, 1, HomeView::kHoldNs);
	EXPECT_FALSE(view.recentered());
	view.Update(Head(false), true, 1, HomeView::kHoldNs + 1);
	EXPECT_EQ(view.recenter_count(), 0u);
}

TEST(HomeView, LongPressCentersHeadAndRequiresReleaseToRepeat)
{
	HomeView view;
	view.Update(Head(), true, 1, 1);
	const auto result = view.Update(Head(), true, 1, HomeView::kHoldNs + 1);
	for (float value: result.position)
		EXPECT_NEAR(value, 0, .00001);
	EXPECT_EQ(view.recenter_count(), 1u);
	view.Update(Head(), true, 1, HomeView::kHoldNs * 10);
	EXPECT_EQ(view.recenter_count(), 1u);
	view.Update(Head(false), true, 1, HomeView::kHoldNs * 10 + 1);
	view.Update(Head(), true, 1, HomeView::kHoldNs * 10 + 2);
	view.Update(Head(), true, 1, HomeView::kHoldNs * 11 + 2);
	EXPECT_EQ(view.recenter_count(), 2u);
}

TEST(HomeView, NeverTransformsAnExperienceOrJoiningFrame)
{
	HomeView view;
	view.Update(Head(), true, 1, 1);
	view.Update(Head(), true, 1, HomeView::kHoldNs + 1);
	const auto result = view.Update(Head(), false, 2, HomeView::kHoldNs * 3);
	EXPECT_FLOAT_EQ(result.position[1], Head().position[1]);
	EXPECT_FALSE(view.recentered());
	EXPECT_EQ(view.recenter_count(), 1u);
}

TEST(HomeView, HomeReturnRequiresNewDeliberatePress)
{
	HomeView view;
	view.Update(Head(), true, 1, 1);
	view.Update(Head(), true, 1, HomeView::kHoldNs + 1);
	const auto result = view.Update(Head(), true, 3, HomeView::kHoldNs * 3);
	EXPECT_FLOAT_EQ(result.position[1], Head().position[1]);
	EXPECT_EQ(view.recenter_count(), 1u);
}

TEST(HomeView, TrackingLossCancelsHold)
{
	HomeView view;
	view.Update(Head(), true, 1, 1);
	auto invalid = Head();
	invalid.valid = false;
	view.Update(invalid, true, 1, HomeView::kHoldNs);
	view.Update(Head(), true, 1, HomeView::kHoldNs + 1);
	EXPECT_FALSE(view.recentered());
}

TEST(HomeView, SleepingControllersCannotRecenter)
{
	HomeView view;
	auto invalid = Head();
	invalid.controllers_connected = false;
	view.Update(invalid, true, 1, 1);
	view.Update(invalid, true, 1, HomeView::kHoldNs * 2);
	EXPECT_FALSE(view.recentered());
}

TEST(HomeView, InvalidCoordinatesCannotBecomeAnOrigin)
{
	HomeView view;
	auto invalid = Head();
	invalid.position[1] = std::numeric_limits<float>::quiet_NaN();
	view.Update(invalid, true, 1, 1);
	view.Update(invalid, true, 1, HomeView::kHoldNs * 2);
	EXPECT_FALSE(view.recentered());
}

TEST(HomeView, RigidTransformPreservesHandAndFloorRelativeDistances)
{
	HomeView view;
	auto sample = Head();
	sample.floor_valid = true;
	sample.floor_position[1] = -1.5f;
	sample.hands[0].grip_position[1] = -1.1f;
	sample.hands[0].aim_position[1] = -.8f;
	view.Update(sample, true, 1, 1);
	const auto result = view.Update(sample, true, 1, HomeView::kHoldNs + 1);
	EXPECT_NEAR(result.floor_position[1] - result.position[1], -.55f, .00001);
	EXPECT_NEAR(result.hands[0].grip_position[1] - result.position[1], -.15f, .00001);
	EXPECT_NEAR(result.hands[0].aim_position[1] - result.position[1], .15f, .00001);
	EXPECT_FLOAT_EQ(result.eye_offset[0][0], sample.eye_offset[0][0]);
	EXPECT_EQ(result.frame, sample.frame);
}

TEST(HomeView, RecentersYawButNotHeadPitch)
{
	HomeView view;
	auto sample = Head();
	using namespace mocktail::vr::pose;
	const auto q = QuatMultiply({0, std::sin(.6f), 0, std::cos(.6f)},
	                            {std::sin(.1f), 0, 0, std::cos(.1f)});
	sample.orientation[0] = q.x;
	sample.orientation[1] = q.y;
	sample.orientation[2] = q.z;
	sample.orientation[3] = q.w;
	view.Update(sample, true, 1, 1);
	const auto result = view.Update(sample, true, 1, HomeView::kHoldNs + 1);
	EXPECT_NEAR(result.orientation[0], std::sin(.1f), .00001);
	EXPECT_NEAR(result.orientation[1], 0, .00001);
	EXPECT_NEAR(result.orientation[3], std::cos(.1f), .00001);
}

TEST(RenderTuning, BalancedAllocatesRealQuestSizedEyes)
{
	const auto tuning = Tuning("balanced");
	EXPECT_EQ(ScaledExtent(1465, 16384, tuning.scale), 1832u);
	EXPECT_EQ(ScaledExtent(1535, 16384, tuning.scale), 1920u);
	EXPECT_EQ(tuning.msaa, 2);
}

TEST(RenderTuning, QualityAndPerformanceAreExplicitChoices)
{
	EXPECT_EQ(Tuning("quality").msaa, 4);
	EXPECT_GT(Tuning("quality").scale, Tuning("balanced").scale);
	EXPECT_EQ(Tuning("performance").scale, 1);
	EXPECT_TRUE(Tuning("legacy").legacy);
	EXPECT_FALSE(Tuning("unknown").valid);
}

TEST(RenderTuning, CannotExceedRuntimeLimits)
{
	EXPECT_EQ(ScaledExtent(1465, 1600, 1.25), 1600u);
}

TEST(RenderTuning, InvalidExtentAndScaleFailClosed)
{
	EXPECT_EQ(ScaledExtent(0, 16384, 1), 0u);
	EXPECT_EQ(ScaledExtent(1000, 0, 1), 0u);
	EXPECT_EQ(ScaledExtent(1000, 16384, 100), 0u);
	EXPECT_EQ(ScaledExtent(1000, 16384, std::numeric_limits<double>::quiet_NaN()), 0u);
}
} // namespace
