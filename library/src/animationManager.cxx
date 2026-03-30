#include "animationManager.h"

#include "interactor_impl.h"
#include "log.h"
#include "macros.h"
#include "options.h"
#include "window_impl.h"

#include "F3DStyle.h"
#include "vtkF3DMetaImporter.h"
#include "vtkF3DRenderer.h"

#include <vtkDoubleArray.h>
#include <vtkProgressBarRepresentation.h>
#include <vtkRenderWindow.h>
#include <vtkRendererCollection.h>
#include <vtkVersion.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = PI * 2.0;

// Non-standard sign of a number (treating 0 as 1 and -0 as -1)
template <typename T> T strictSign(T val)
{
  if (std::signbit(val)) { return -1; }
  return 1;
}

f3d::vector3_t getCamUpPrincipleAxes(const f3d::vector3_t& camUp)
{
  // Locate the highest magnitude element (index) as the "up" axis
  const auto maxUpElem = std::max_element(camUp.begin(), camUp.end(),
    [](const double A, const double B) {
        return std::abs(A) < std::abs(B);
    });
  const int maxUpIndex = std::distance(camUp.begin(), maxUpElem);
  const bool upIsNeg = (camUp[maxUpIndex] < 0);

  // Debugging output
  // char upAxisName = 'X';
  // if (maxUpIndex == 1) { upAxisName = 'Y'; }
  // else if (maxUpIndex == 2) { upAxisName = 'Z'; }
  // f3d::log::debug("Up Axis: ", (upIsNeg ? "-" : ""), upAxisName);

  // Determine coordinate sys orientation from up axis
  f3d::vector3_t axisOut = {};
  switch (maxUpIndex)
  {
    default:
    case 0:
      if (upIsNeg) axisOut = { -2.0, 1.0, 0.0 };
      else axisOut = { 2.0, 1.0, -0.0 };
      break;

    case 1:
      if (upIsNeg) axisOut = { 0.0, -2.0, 1.0 };
      else axisOut = { 0.0, 2.0, -1.0 };
      break;

    case 2:
      if (upIsNeg) axisOut = { 0.0, -1.0, 2.0 };
      else axisOut = { 0.0, 1.0, 2.0 };
      break;
  }

  return axisOut;
}

f3d::point3_t cartesianToCylindricalForAxis(const f3d::point3_t& point, const f3d::vector3_t& axis)
{
  // Remap coordinates using axis
  const f3d::point3_t localPos = {
    point[static_cast<int>(std::abs(axis[0]))] * strictSign(axis[0]),
    point[static_cast<int>(std::abs(axis[1]))] * strictSign(axis[1]),
    point[static_cast<int>(std::abs(axis[2]))] * strictSign(axis[2]),
  };

  f3d::log::debug("Pre Cyl coords: (", localPos[0], ", ", localPos[1], ", ", localPos[2], ")");

  // Convert to cylindrical coordinates (along local +Z)
  const double radius = sqrt(localPos[0] * localPos[0] + localPos[1] * localPos[1]);
  if (radius > 1e-6)
  {
    // Adjust theta for orbit
    const double theta = atan2(localPos[1], localPos[0]);
    return { radius, theta, localPos[2] };
  }

  // If radius is too small, leave theta as 0.0
  return { radius, 0.0, localPos[2] };
}

f3d::point3_t cylindricalToCartesianForAxis(const f3d::point3_t& point, const f3d::vector3_t& axis)
{
  // Back to cartesian coordinates
  const f3d::point3_t localPos = {
    point[0] * cos(point[1]),
    point[0] * sin(point[1]),
    point[2]
  };

  // Remap using axis
  f3d::point3_t globalPos = {};
  globalPos[static_cast<int>(std::abs(axis[0]))] = localPos[0] * strictSign(axis[0]);
  globalPos[static_cast<int>(std::abs(axis[1]))] = localPos[1] * strictSign(axis[1]);
  globalPos[static_cast<int>(std::abs(axis[2]))] = localPos[2] * strictSign(axis[2]);
  return globalPos;
}

namespace f3d::detail
{
//----------------------------------------------------------------------------
animationManager::animationManager(options& options, window_impl& window)
  : Options(options)
  , Window(window)
{
}

//----------------------------------------------------------------------------
void animationManager::SetImporter(vtkF3DMetaImporter* importer)
{
  this->Importer = importer;
}

//----------------------------------------------------------------------------
void animationManager::SetInteractor(interactor_impl* interactor)
{
  this->Interactor = interactor;
}

//----------------------------------------------------------------------------
void animationManager::SetDeltaTime(double deltaTime)
{
  this->DeltaTime = deltaTime;
}

//----------------------------------------------------------------------------
void animationManager::Initialize()
{
  assert(this->Importer);
  this->Playing = false;
  this->CurrentTime = 0;
  this->CurrentTimeSet = false;

  this->AvailAnimations = this->Importer->GetNumberOfAnimations();
  if (this->AvailAnimations > 0 && this->Interactor)
  {
    this->ProgressWidget = vtkSmartPointer<vtkProgressBarWidget>::New();
    this->Interactor->SetInteractorOn(this->ProgressWidget);

    vtkProgressBarRepresentation* progressRep =
      vtkProgressBarRepresentation::SafeDownCast(this->ProgressWidget->GetRepresentation());
    progressRep->SetProgressRate(0.0);
    progressRep->ProportionalResizeOff();
    progressRep->SetPosition(0.0, 0.0);
    progressRep->SetPosition2(1.0, 0.0);
    progressRep->SetMinimumSize(0, 5);
    f3d::color_t color;
    if (!this->Options.ui.animation_progress_color.has_value())
    {
      const auto [r, g, b] = F3DStyle::GetF3DBlue();
      color = color_t(r, g, b);
    }
    else
    {
      color = this->Options.ui.animation_progress_color.value();
    }
    progressRep->SetProgressBarColor(color.r(), color.g(), color.b());
    progressRep->DrawBackgroundOff();
    progressRep->DragableOff();
    progressRep->SetShowBorderToOff();
    progressRep->DrawFrameOff();
    progressRep->SetPadding(0.0, 0.0);
    progressRep->SetVisibility(this->Options.ui.animation_progress);
    this->ProgressWidget->On();
  }
  else
  {
    this->ProgressWidget = nullptr;
  }

  // Reset animation indices before updating
  this->PreparedAnimationIndices.reset();
  this->AnimationTimeSteps->Reset();
  this->PrepareForAnimationIndices();

  if (this->AvailAnimations == 0)
  {
    log::debug("No animation available");
    return;
  }
  else
  {
    log::debug("Animation(s) available are:");
  }

  for (int i = 0; i < this->AvailAnimations; i++)
  {
    log::debug(i, ": ", this->Importer->GetAnimationName(i));
  }

  if (this->Autoplay)
  {
    this->StartAnimation();
  }
}

//----------------------------------------------------------------------------
void animationManager::StartAnimation()
{
  if (!this->IsPlaying())
  {
    this->ToggleAnimation();
  }
}

//----------------------------------------------------------------------------
void animationManager::StopAnimation()
{
  if (this->IsPlaying())
  {
    this->ToggleAnimation();
  }
}

//----------------------------------------------------------------------------
void animationManager::ToggleAnimation()
{
  this->PrepareForAnimationIndices();
  if (!this->PreparedAnimationIndices.value().empty() && this->Interactor)
  {
    this->Playing = !this->Playing;

    if (this->Playing)
    {
      // Initialize time if not already
      if (!this->CurrentTimeSet)
      {
        this->CurrentTime = this->TimeRange[0];
        this->CurrentTimeSet = true;
      }
    }

    if (this->Playing && this->Options.scene.camera.index.has_value())
    {
      this->Interactor->disableCameraMovement();
    }
    else
    {
      this->Interactor->enableCameraMovement();
    }
  }
}

//----------------------------------------------------------------------------
void animationManager::ToggleCameraOrbit()
{
    this->Orbiting = !this->Orbiting;
}

//----------------------------------------------------------------------------
void animationManager::StartCameraOrbit()
{
  if (!this->Orbiting)
  {
    this->ToggleCameraOrbit();
  }
}

//----------------------------------------------------------------------------
void animationManager::StopCameraOrbit()
{
  if (this->Orbiting)
  {
    this->ToggleCameraOrbit();
  }
}

//----------------------------------------------------------------------------
bool animationManager::Tick()
{
  assert(this->DeltaTime > 0);
  bool updateNeeded = false;
  if (this->Playing)
  {
    updateNeeded = updateNeeded || this->TickAnimation();
  }

  if (this->Orbiting && this->Options.scene.camera.orbit.has_value() && fabs(this->Options.scene.camera.orbit.value()) > 1e-6)
  {
    updateNeeded = updateNeeded || this->TickCameraOrbit();
  }

  return updateNeeded;
}

bool animationManager::TickAnimation()
{
  // Update animation time
  this->CurrentTime += (this->DeltaTime * this->SpeedFactor) * this->AnimationDirection;

  // Modulo computation, compute CurrentTime in the time range.
  if (this->CurrentTime < this->TimeRange[0] || this->CurrentTime > this->TimeRange[1])
  {
    auto modulo = [](double val, double mod)
    {
      const double remainder = fmod(val, mod);
      return remainder < 0 ? remainder + mod : remainder;
    };
    this->CurrentTime = this->TimeRange[0] +
      modulo(this->CurrentTime - this->TimeRange[0], this->TimeRange[1] - this->TimeRange[0]);
  }

  // Advance the animation
  return this->LoadAtTime(this->CurrentTime);
}

bool animationManager::TickCameraOrbit()
{
  // Get camera location and focus
  camera& cam = this->Window.getCamera();
  const point3_t camFocus = cam.getFocalPoint();
  const point3_t camCurPos = cam.getPosition();
  const point3_t reCenteredCamPos = {
    camCurPos[0] - camFocus[0],
    camCurPos[1] - camFocus[1],
    camCurPos[2] - camFocus[2]
  };

  // Compute principle axes of camera view (from the camera "up" axis)
  const vector3_t camUp = cam.getViewUp();
  const vector3_t axis = getCamUpPrincipleAxes(cam.getViewUp());

  // Compute camera location in cylindrical coordinates around the given principle axes
  f3d::log::debug("Global coords: (", reCenteredCamPos[0], ", ", reCenteredCamPos[1], ", ", -reCenteredCamPos[2], ")");
  if (point3_t cylinderCoords = cartesianToCylindricalForAxis(reCenteredCamPos, axis);
    std::abs(cylinderCoords[0]) > 1e-6)
  {
    f3d::log::debug("    > Cylinder: ", cylinderCoords[0], "r, ", vtkMath::DegreesFromRadians(cylinderCoords[1]), "deg, ", cylinderCoords[2]);
    const double oldTheta = cylinderCoords[1];

    // adjust the theta angle to orbit (we subtract to get more natural rotation direction)
    const double percentChange = this->DeltaTime / this->Options.scene.camera.orbit.value();
    cylinderCoords[1] -= percentChange * this->SpeedFactor * TWO_PI;
    while (cylinderCoords[1] > PI) { cylinderCoords[1] -= TWO_PI; }
    while (cylinderCoords[1] < -PI) { cylinderCoords[1] += TWO_PI; }

    // Back to cartesian coordinates
    const point3_t newCamPos = cylindricalToCartesianForAxis(cylinderCoords, axis);
    cam.setPosition({
      newCamPos[0] + camFocus[0],
      newCamPos[1] + camFocus[1],
      newCamPos[2] + camFocus[2]
    });

    // New viewing vector
    vector3_t newView = {
      camFocus[0] - cam.getPosition()[0],
      camFocus[1] - cam.getPosition()[1],
      camFocus[2] - cam.getPosition()[2]
    };
    vtkMath::Normalize(newView.data());

    // Re-compute proper co-linear axes
    vector3_t newRight = {};
    vtkMath::Cross(newView, cam.getViewUp(), newRight);

    vector3_t newUp = {};
    vtkMath::Cross(newRight, newView, newUp);

    cam.setViewUp(newUp);

    return true;
  }

  return false;
}

//----------------------------------------------------------------------------
void animationManager::JumpToFrame(int frame, bool relative)
{
  assert(this->DeltaTime > 0);
  const double frameDuration = (this->DeltaTime * this->SpeedFactor);
  const double currentFrame = (this->CurrentTime - this->TimeRange[0]) / frameDuration;

  double nextFrame = 0;
  if (relative)
  {
    nextFrame = currentFrame + frame;
  }
  else if (frame >= 0)
  {
    nextFrame = frame;
  }
  else
  {
    nextFrame = (this->TimeRange[1] - this->TimeRange[0]) / frameDuration;
  }

  this->CurrentTime = this->TimeRange[0] + (nextFrame * this->DeltaTime * this->SpeedFactor);

  if (this->LoadAtTime(this->CurrentTime))
  {
    this->Window.render();
  }
}

//----------------------------------------------------------------------------
void animationManager::JumpToKeyFrame(int keyframe, bool relative)
{
  if (this->AnimationTimeSteps->GetNumberOfTuples() == 0)
  {
    return;
  }

  const int timeStepsAvailable = this->AnimationTimeSteps->GetNumberOfTuples();

  auto it = std::lower_bound(
    this->AnimationTimeSteps->Begin(), this->AnimationTimeSteps->End(), this->CurrentTime);
  const int closestKeyFrame = (it != this->AnimationTimeSteps->End())
    ? static_cast<int>(std::distance(this->AnimationTimeSteps->Begin(), it))
    : timeStepsAvailable - 1;

  int nextKeyFrame = closestKeyFrame;
  if (relative)
  {
    nextKeyFrame += keyframe;
    nextKeyFrame = ((nextKeyFrame % timeStepsAvailable) + timeStepsAvailable) % timeStepsAvailable;
  }
  else
  {
    nextKeyFrame = keyframe > 0 ? std::min(keyframe, timeStepsAvailable - 1) : 0;
    if (0 > keyframe || keyframe > timeStepsAvailable)
    {
      log::warn("Keyframe index ", keyframe, " is outside of range [0-", timeStepsAvailable - 1,
        "], converting to ", nextKeyFrame, " instead.");
    }
  }

  this->CurrentTime = this->AnimationTimeSteps->GetValue(nextKeyFrame);

  if (this->LoadAtTime(this->CurrentTime))
  {
    this->Window.render();
  }
}

//----------------------------------------------------------------------------
bool animationManager::LoadAtTime(double timeValue)
{
  assert(this->Importer);

  if (this->AvailAnimations == 0)
  {
    log::warn("No animation available, cannot load a specific animation time");
    this->Playing = false;
    return false;
  }

  this->PrepareForAnimationIndices();
  if (this->PreparedAnimationIndices.value().empty())
  {
    return false;
  }

  /* clamp target time to available range */
  // 1 microsecond tolerance so we don't log messages if times are insignificantly close
  constexpr double epsilon = 1e-6;
  if (timeValue < this->TimeRange[0])
  {
    if (this->TimeRange[0] - timeValue > epsilon)
    {
      log::warn("Animation time ", timeValue, " is outside of range [", this->TimeRange[0], ", ",
        this->TimeRange[1], "], using ", this->TimeRange[0], ".");
    }
    timeValue = this->TimeRange[0];
  }
  else if (timeValue > this->TimeRange[1])
  {
    if (timeValue - this->TimeRange[1] > epsilon)
    {
      log::warn("Animation time ", timeValue, " is outside of range [", this->TimeRange[0], ", ",
        this->TimeRange[1], "], using ", this->TimeRange[1], ".");
    }
    timeValue = this->TimeRange[1];
  }
  this->CurrentTime = timeValue;
  this->CurrentTimeSet = true;
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 3, 20240707)
  if (!this->Importer->UpdateAtTimeValue(this->CurrentTime))
  {
    log::error("Could not load time value: ", this->CurrentTime);
    return false;
  }
#else
  this->Importer->UpdateTimeStep(this->CurrentTime);
#endif

  if (this->Interactor && this->ProgressWidget)
  {
    // Set progress bar
    vtkProgressBarRepresentation* progressRep =
      vtkProgressBarRepresentation::SafeDownCast(this->ProgressWidget->GetRepresentation());
    progressRep->SetProgressRate(
      (this->CurrentTime - this->TimeRange[0]) / (this->TimeRange[1] - this->TimeRange[0]));

    this->Interactor->UpdateRendererAfterInteraction();
  }
  return true;
}

// ---------------------------------------------------------------------------------
void animationManager::CycleAnimation()
{
  assert(this->Importer);
  if (this->AvailAnimations == 0)
  {
    return;
  }

  // F3D_DEPRECATED
  // Remove this in the next major release
  F3D_SILENT_WARNING_PUSH()
  F3D_SILENT_WARNING_DECL(4996, "deprecated-declarations")
  if (this->Options.scene.animation.indices == std::vector<int>{ 0 } &&
    this->Options.scene.animation.index != 0)
  {
    log::warn("scene.animation.index is deprecated, please use "
              "scene.animation.indices instead");
    this->Options.scene.animation.indices = { this->Options.scene.animation.index };
    this->Options.scene.animation.index = 0;
  }
  F3D_SILENT_WARNING_POP()

  // If we started with multi animation or all animations (any negative value means all animations)
  bool negative = std::any_of(this->Options.scene.animation.indices.begin(),
    this->Options.scene.animation.indices.end(), [](int idx) { return idx < 0; });
  if (this->Options.scene.animation.indices.size() > 1 || negative)
  {
    // Then select no animation
    this->Options.scene.animation.indices.clear();
  }
  // If no animation selected
  else if (this->Options.scene.animation.indices.empty())
  {
    // Select the first one
    this->Options.scene.animation.indices = { 0 };
  }
  else
  {
    // If there was only one animation selected, then increment animation index
    this->Options.scene.animation.indices[0]++;

    // If we reach/exceeded the last animation
    if (this->Options.scene.animation.indices[0] >= this->AvailAnimations)
    {
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 4, 20250507)
      // If importer support multi animations and there are multiple animations
      if (this->Importer->GetAnimationSupportLevel() == vtkImporter::AnimationSupportLevel::MULTI &&
        this->AvailAnimations > 1)
#else
      if (this->AvailAnimations > 1)
#endif
      {
        // Then select all
        this->Options.scene.animation.indices.resize(this->AvailAnimations);
        std::iota(this->Options.scene.animation.indices.begin(),
          this->Options.scene.animation.indices.end(), 0);
      }
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 4, 20250507)
      else
      {
        // If not, select none
        this->Options.scene.animation.indices.clear();
      }
#endif
    }
  }

  this->PrepareForAnimationIndices();
  this->LoadAtTime(this->TimeRange[0]);

  vtkRenderWindow* renWin = this->Window.GetRenderWindow();
  vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
  ren->SetCheatSheetConfigured(false);
}

// ---------------------------------------------------------------------------------
std::string animationManager::GetAnimationName(int index)
{
  assert(this->Importer);
  if (index == -1)
  {
    if (this->PreparedAnimationIndices.has_value() &&
      this->PreparedAnimationIndices.value().size() > 1)
    {
      std::vector<bool> animCheck(this->AvailAnimations, false);
      for (int idx : this->PreparedAnimationIndices.value())
      {
        if (idx < this->AvailAnimations)
        {
          animCheck[idx] = true;
        }
      }
      return std::none_of(animCheck.begin(), animCheck.end(), std::logical_not<>())
        ? "All animations"
        : "Multi animations";
    }

    if (this->AvailAnimations == 0 || !this->PreparedAnimationIndices.has_value() ||
      this->PreparedAnimationIndices.value().empty() ||
      this->PreparedAnimationIndices.value()[0] >= this->AvailAnimations)
    {
      return "No animation";
    }

    return this->Importer->GetAnimationName(this->PreparedAnimationIndices.value()[0]);
  }

  if (this->AvailAnimations == 0 || index < 0 || index > this->AvailAnimations)
  {
    return "No animation";
  }

  return this->Importer->GetAnimationName(index);
}

// ---------------------------------------------------------------------------------
std::vector<std::string> animationManager::GetAnimationNames()
{
  assert(this->Importer);

  if (this->AvailAnimations == 0)
  {
    return {};
  }

  std::vector<std::string> animations(this->AvailAnimations);

  for (int index = 0; index < this->AvailAnimations; index++)
  {
    animations[index] = this->Importer->GetAnimationName(index);
  }

  return animations;
}

//----------------------------------------------------------------------------
void animationManager::PrepareForAnimationIndices()
{
  assert(this->Importer);

  std::vector<int> animIndices = this->Options.scene.animation.indices;

  // F3D_DEPRECATED
  // Remove this in the next major release
  F3D_SILENT_WARNING_PUSH()
  F3D_SILENT_WARNING_DECL(4996, "deprecated-declarations")
  if (animIndices == std::vector<int>{ 0 } && this->Options.scene.animation.index != 0)
  {
    log::warn("scene.animation.index is deprecated, please use "
              "scene.animation.indices instead");
    animIndices = { this->Options.scene.animation.index };
  }
  F3D_SILENT_WARNING_POP()

  // If it contains a negative value, all animations should be selected
  if (std::any_of(animIndices.begin(), animIndices.end(), [](int idx) { return idx < 0; }))
  {
    if (animIndices.size() > 1)
    {
      log::warn("Multiple animation indices have been specified include a negative one, all "
                "animations will be selected");
    }

    animIndices.resize(this->AvailAnimations);
    std::iota(animIndices.begin(), animIndices.end(), 0);
  }

  if (this->PreparedAnimationIndices.has_value() &&
    this->PreparedAnimationIndices.value() == animIndices)
  {
    // Already updated
    return;
  }

  // Do not warn at all if default or empty
  if (!animIndices.empty() && animIndices != std::vector<int>{ 0 })
  {
    if (this->AvailAnimations == 0)
    {
      log::warn(
        "Animation indices have been specified but there are no animation available in this file.");
    }
    else
    {
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 4, 20250507)
      switch (this->Importer->GetAnimationSupportLevel())
      {
        case vtkImporter::AnimationSupportLevel::UNIQUE:
          if (this->Options.scene.animation.indices[0] != 0 ||
            this->Options.scene.animation.indices.size() > 1)
          {
            log::warn("Non-zero or multiple animation indices have been specified but currently "
                      "loaded file does not support it.");
          }
          break;
        case vtkImporter::AnimationSupportLevel::SINGLE:
          if (this->Options.scene.animation.indices.size() > 1)
          {
            log::warn(
              "Multiple animation indices have been specified but currently loaded files may "
              "not support enabling multiple animations");
          }
          break;
        default:
          // NONE is unreachable
          // MULTI there is nothing to warn about
          break;
      }
#endif
    }
  }

  this->PreparedAnimationIndices = animIndices;

  if (this->AvailAnimations == 0)
  {
    return;
  }

  // Disable all animations
  for (int idx = 0; idx < this->AvailAnimations; idx++)
  {
    this->Importer->DisableAnimation(idx);
  }

  // Enable the selected ones
  for (int idx : this->PreparedAnimationIndices.value())
  {
    if (idx >= this->AvailAnimations)
    {
      log::warn("Specified animation index: ", idx, " is not in range [0, ",
        this->AvailAnimations - 1, "], ignoring");
    }
    this->Importer->EnableAnimation(idx);
  }

  // Display currently selected animation
  log::debug("Current animation is: ", this->GetAnimationName());

  // Recover time ranges for all enabled animations
  bool foundAnimation = false;
  this->TimeRange[0] = std::numeric_limits<double>::infinity();
  this->TimeRange[1] = -std::numeric_limits<double>::infinity();
  std::set<double> accumulatedTimeSteps;
  for (vtkIdType animIndex = 0; animIndex < this->AvailAnimations; animIndex++)
  {
    if (this->Importer->IsAnimationEnabled(animIndex))
    {
      double timeRange[2];
      int nbTimeSteps;
      this->Importer->GetTemporalInformation(
        animIndex, timeRange, nbTimeSteps, this->AnimationTimeSteps);

      // Accumulate timesteps to avoid overwrite
      for (vtkIdType stepIndex = 0; stepIndex < this->AnimationTimeSteps->GetNumberOfTuples();
           stepIndex++)
      {
        accumulatedTimeSteps.emplace(this->AnimationTimeSteps->GetValue(stepIndex));
      }

      // Accumulate time ranges
      this->TimeRange[0] = std::min(timeRange[0], this->TimeRange[0]);
      this->TimeRange[1] = std::max(timeRange[1], this->TimeRange[1]);
      foundAnimation = true;
    }
  }

  if (foundAnimation)
  {
    // Populate AnimationTimeSteps with accumulated values
    this->AnimationTimeSteps->Reset();
    int nbAccumulatedTimeSteps = static_cast<int>(accumulatedTimeSteps.size());
    this->AnimationTimeSteps->SetNumberOfTuples(nbAccumulatedTimeSteps);
    int index = 0;
    for (double timeStep : accumulatedTimeSteps)
    {
      this->AnimationTimeSteps->SetValue(index, timeStep);
      index++;
    }

    // Check time range is valid
    if (this->TimeRange[0] > this->TimeRange[1])
    {
      log::warn("Animation(s) time range delta is invalid: [", this->TimeRange[0], ", ",
        this->TimeRange[1], "]. Swapping range.");
      std::swap(this->TimeRange[0], this->TimeRange[1]);
    }
    log::debug(
      "Current animation time range is: [", this->TimeRange[0], ", ", this->TimeRange[1], "].");
  }

  log::debug("");
}

//----------------------------------------------------------------------------
std::pair<double, double> animationManager::GetTimeRange()
{
  // Make sure TimeRange is updated
  this->PrepareForAnimationIndices();

  // Return updated data
  return std::make_pair(this->TimeRange[0], this->TimeRange[1]);
}

//----------------------------------------------------------------------------
std::vector<double> animationManager::GetKeyFrames()
{
  this->PrepareForAnimationIndices();

  std::vector<double> keyFrames;
  keyFrames.reserve(this->AnimationTimeSteps->GetNumberOfTuples());

  for (vtkIdType i = 0; i < this->AnimationTimeSteps->GetNumberOfTuples(); ++i)
  {
    keyFrames.push_back(this->AnimationTimeSteps->GetValue(i));
  }

  return keyFrames;
}

//----------------------------------------------------------------------------
unsigned int animationManager::GetNumberOfAvailableAnimations() const
{
  assert(this->AvailAnimations >= 0);
  return static_cast<unsigned int>(this->AvailAnimations);
}

//----------------------------------------------------------------------------
void animationManager::SetCheatSheetConfigured(bool configured)
{
  vtkF3DRenderer* ren = this->Window.GetRenderer();
  ren->SetCheatSheetConfigured(configured);
}

//----------------------------------------------------------------------------
void animationManager::SetAutoplay(bool enable)
{
  if (this->Autoplay != enable)
  {
    this->Autoplay = enable;
    this->SetCheatSheetConfigured(false);
  }
}

//----------------------------------------------------------------------------
void animationManager::SetSpeedFactor(double speedFactor)
{
  if (this->SpeedFactor != speedFactor)
  {
    this->SpeedFactor = speedFactor;
    this->SetCheatSheetConfigured(false);
  }
}

//----------------------------------------------------------------------------
void animationManager::SetAnimationDirection(int direction)
{
  assert(direction == 1 || direction == -1);
  this->AnimationDirection = direction;
}

//----------------------------------------------------------------------------
void animationManager::UpdateDynamicOptions()
{
  this->SetAutoplay(this->Options.scene.animation.autoplay);
  this->SetSpeedFactor(this->Options.scene.animation.speed_factor);
}
}
