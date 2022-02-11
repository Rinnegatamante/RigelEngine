/* Copyright (C) 2017, Nikolai Wuttke. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dynamic_geometry_system.hpp"

#include "base/spatial_types_printing.hpp"
#include "data/map.hpp"
#include "data/sound_ids.hpp"
#include "engine/base_components.hpp"
#include "engine/collision_checker.hpp"
#include "engine/entity_tools.hpp"
#include "engine/life_time_components.hpp"
#include "engine/motion_smoothing.hpp"
#include "engine/physical_components.hpp"
#include "engine/random_number_generator.hpp"
#include "frontend/game_service_provider.hpp"
#include "game_logic/actor_tag.hpp"
#include "game_logic/behavior_controller.hpp"
#include "game_logic/damage_components.hpp"
#include "game_logic/dynamic_geometry_components.hpp"
#include "game_logic/global_dependencies.hpp"
#include "game_logic/ientity_factory.hpp"


namespace rigel::game_logic
{

using game_logic::components::MapGeometryLink;

namespace
{

constexpr auto GEOMETRY_FALL_SPEED = 2;

const base::Vec2f TILE_DEBRIS_MOVEMENT_SEQUENCE[] = {
  {0.0f, -3.0f},
  {0.0f, -3.0f},
  {0.0f, -2.0f},
  {0.0f, -2.0f},
  {0.0f, -1.0f},
  {0.0f, 0.0f},
  {0.0f, 0.0f},
  {0.0f, 1.0f},
  {0.0f, 2.0f},
  {0.0f, 2.0f},
  {0.0f, 3.0f}};


void spawnTileDebris(
  entityx::EntityManager& entities,
  const int x,
  const int y,
  const data::map::TileIndex tileIndex,
  const int velocityX,
  const int ySequenceOffset)
{
  using namespace engine::components;
  using namespace engine::components::parameter_aliases;
  using components::TileDebris;

  auto movement = MovementSequence{
    TILE_DEBRIS_MOVEMENT_SEQUENCE, ResetAfterSequence{false}, EnableX{false}};
  movement.mCurrentStep = ySequenceOffset;

  auto debris = entities.create();
  debris.assign<WorldPosition>(x, y);
  debris.assign<BoundingBox>(BoundingBox{{}, {1, 1}});
  debris.assign<Active>();
  debris.assign<ActivationSettings>(ActivationSettings::Policy::Always);
  debris.assign<AutoDestroy>(AutoDestroy::afterTimeout(80));
  debris.assign<TileDebris>(TileDebris{tileIndex});
  debris.assign<MovingBody>(
    Velocity{static_cast<float>(velocityX), 0.0f},
    GravityAffected{false},
    IgnoreCollisions{true});
  debris.assign<MovementSequence>(movement);
  engine::enableInterpolation(debris);
}


void spawnTileDebrisForSection(
  const engine::components::BoundingBox& mapSection,
  const data::map::Map& map,
  entityx::EntityManager& entities,
  engine::RandomNumberGenerator& randomGen)
{
  const auto& start = mapSection.topLeft;
  const auto& size = mapSection.size;
  for (auto y = start.y; y < start.y + size.height; ++y)
  {
    for (auto x = start.x; x < start.x + size.width; ++x)
    {
      const auto tileIndex = map.tileAt(0, x, y);
      if (tileIndex == 0)
      {
        continue;
      }

      const auto velocityX = 3 - randomGen.gen() % 6;
      const auto ySequenceOffset = randomGen.gen() % 5;
      spawnTileDebris(entities, x, y, tileIndex, velocityX, ySequenceOffset);
    }
  }
}


void explodeMapSection(
  const base::Rect<int>& mapSection,
  data::map::Map& map,
  entityx::EntityManager& entityManager,
  entityx::EventManager& eventManager,
  engine::RandomNumberGenerator& randomGenerator)
{
  spawnTileDebrisForSection(mapSection, map, entityManager, randomGenerator);

  map.clearSection(
    mapSection.topLeft.x,
    mapSection.topLeft.y,
    mapSection.size.width,
    mapSection.size.height);
}


void explodeMapSection(
  const base::Rect<int>& mapSection,
  GlobalDependencies& d,
  GlobalState& s)
{
  explodeMapSection(
    mapSection,
    *s.mpMap,
    *d.mpEntityManager,
    *d.mpEvents,
    *d.mpRandomGenerator);
}


void moveTileRows(const base::Rect<int>& mapSection, data::map::Map& map)
{
  const auto startX = mapSection.left();
  const auto startY = mapSection.top();
  const auto width = mapSection.size.width;
  const auto height = mapSection.size.height;

  for (int layer = 0; layer < 2; ++layer)
  {
    for (int row = 0; row < height; ++row)
    {
      for (int col = 0; col < width; ++col)
      {
        const auto x = startX + col;
        const auto sourceY = startY + (height - row - 1);
        const auto targetY = sourceY + 1;

        map.setTileAt(layer, x, targetY, map.tileAt(layer, x, sourceY));
      }
    }
  }

  map.clearSection(startX, startY, width, 1);
}


void moveTileSection(base::Rect<int>& mapSection, data::map::Map& map)
{
  moveTileRows(mapSection, map);
  ++mapSection.topLeft.y;
}


void squashTileSection(base::Rect<int>& mapSection, data::map::Map& map)
{
  // By not moving the lower-most row, it gets effectively overwritten by the
  // row above
  moveTileRows(
    {mapSection.topLeft, {mapSection.size.width, mapSection.size.height - 1}},
    map);
  ++mapSection.topLeft.y;
  --mapSection.size.height;
}

} // namespace


// This function splits the map up into a static part, which we hand over
// to the MapRenderer, and dynamic parts. The dynamic parts can change during
// gameplay and thus cannot be rendered as static VBOs, but rather have to
// be rendered dynamically (see DynamicGeometrySystem::renderDynamicSections).
DynamicMapSectionData determineDynamicMapSections(
  const data::map::Map& originalMap,
  const std::vector<data::map::LevelData::Actor>& actorDescriptions)
{
  DynamicMapSectionData result{originalMap, {}, {}};
  auto& map = result.mMapStaticParts;

  // We don't have entities yet, but the CollisionChecker needs them.
  // To avoid making the CollisionChecker more complex, we simply create
  // a dummy EntityManager here.
  entityx::EventManager dummyEvents;
  entityx::EntityManager dummyEntities{dummyEvents};
  engine::CollisionChecker checker{&map, dummyEntities, dummyEvents};

  auto findSectionBelowFallingSection =
    [&](const base::Rect<int>& section) -> std::optional<std::tuple<int, int>> {
    for (auto y = section.bottom() + 1; y < map.height() &&
         !checker.isOnSolidGround(
           {{section.left(), y - 1}, {section.size.width, 1}});
         ++y)
    {
      for (auto x = section.left(); x < section.left() + section.size.width;
           ++x)
      {
        if (map.tileAt(0, x, y) != 0 || map.tileAt(1, x, y) != 0)
        {
          auto y2 = y + 1;
          while (y2 < map.height() &&
                 !checker.isOnSolidGround(
                   {{section.left(), y2 - 1}, {section.size.width, 1}}))
          {
            ++y2;
          }

          return std::tuple{y, y2};
        }
      }
    }

    return std::nullopt;
  };


  std::vector<base::Rect<int>> dynamicSections;

  for (const auto& actor : actorDescriptions)
  {
    if (actor.mAssignedArea)
    {
      const auto& section = *actor.mAssignedArea;

      // Type 2 (shootable wall) is the only type that can't fall down
      if (actor.mID == data::ActorID::Dynamic_geometry_2)
      {
        map.clearSection(
          section.left(),
          section.top(),
          section.size.width,
          section.size.height);
      }
      else
      {
        // The other types of dynamic geometry are handled separately
        dynamicSections.push_back(section);
      }
    }
    else
    {
      // Missiles can explode parts of the map
      if (actor.mID == data::ActorID::Missile_intact)
      {
        const auto x = actor.mPosition.x;
        auto y = actor.mPosition.y - 12;
        while (y >= 0 && !checker.isTouchingCeiling({{x, y + 1}, {3, 1}}))
        {
          --y;
        }

        if (y >= 2)
        {
          map.clearSection(x, y - 2, 3, 3);
          result.mSimpleSections.push_back({{x, y - 2}, {3, 3}});
        }
      }
    }
  }

  // Burnable tiles
  for (auto y = 0; y < map.height(); ++y)
  {
    for (auto x = 0; x < map.width(); ++x)
    {
      if (map.attributes(x, y).isFlammable())
      {
        auto endX = x + 1;
        while (endX < map.width() && map.attributes(endX, y).isFlammable())
        {
          ++endX;
        }

        auto endY = y + 1;
        while (endY < map.height() && map.attributes(x, endY).isFlammable())
        {
          ++endY;
        }

        const auto section = base::Rect<int>{{x, y}, {endX - x, endY - y}};
        map.clearSection(x, y, section.size.width, section.size.height);
        result.mSimpleSections.push_back(section);
      }
    }
  }

  // Sections below dynamic (falling) geometry
  auto index = 0;
  for (const auto& section : dynamicSections)
  {
    if (const auto areaBelow = findSectionBelowFallingSection(section))
    {
      const auto [top, bottom] = *areaBelow;
      result.mFallingSections.push_back(
        {{{section.left(), top}, {section.size.width, bottom - top}}, index});
    }

    ++index;
  }

  for (const auto& section : dynamicSections)
  {
    map.clearSection(
      section.left(), section.top(), section.size.width, section.size.height);
  }

  for (const auto& [section, _] : result.mFallingSections)
  {
    map.clearSection(
      section.left(), section.top(), section.size.width, section.size.height);
  }

  return result;
}


DynamicGeometrySystem::DynamicGeometrySystem(
  IGameServiceProvider* pServiceProvider,
  entityx::EntityManager* pEntityManager,
  data::map::Map* pMap,
  engine::RandomNumberGenerator* pRandomGenerator,
  entityx::EventManager* pEvents,
  engine::MapRenderer* pMapRenderer,
  std::vector<base::Rect<int>> simpleDynamicSections)
  : mpServiceProvider(pServiceProvider)
  , mpEntityManager(pEntityManager)
  , mpMap(pMap)
  , mpRandomGenerator(pRandomGenerator)
  , mpEvents(pEvents)
  , mpMapRenderer(pMapRenderer)
  , mSimpleDynamicSections(std::move(simpleDynamicSections))
{
  pEvents->subscribe<events::ShootableKilled>(*this);
  pEvents->subscribe<rigel::events::DoorOpened>(*this);
  pEvents->subscribe<rigel::events::MissileDetonated>(*this);
  pEvents->subscribe<rigel::events::TileBurnedAway>(*this);
}


void DynamicGeometrySystem::initializeDynamicGeometryEntities(
  const std::vector<FallingSectionInfo>& fallingSections)
{
  auto iFallingSectionInfo = fallingSections.begin();

  // Entities for the level have already been created, so we now have one
  // entity for each piece of dynamic geometry. We now need to go through these
  // and assign the data for the area below the dynamic geometry that might be
  // affected when the former is falling down. The affected area has already
  // been determined in determineDynamicMapSections, but at the time when we
  // did that we didn't yet have entities in the map.  We rely on the fact here
  // that the order in which we create entities matches the order in which they
  // appear in the level, and that EntityX preserves the order of creation. So
  // we keep a running index, and use it to match the entities up with the
  // corresponding entries in the fallingSections vector.
  mpEntityManager->each<MapGeometryLink>(
    [&, index = 0](entityx::Entity entity, MapGeometryLink& link) mutable {
      // Shootable walls are dynamic geometry, but they do not fall down,
      // hence they are not relevant here.
      if (entity.has_component<components::Shootable>())
      {
        return;
      }

      // We have found a corresponding entry matching the entity we are
      // currently looking at, so we need to grab a copy of the map data
      // and attach it to the extraSection data of the entity. This allows
      // us to render the area below a falling piece of geometry correctly,
      // while also rendering the moving geometry itself smoothly.
      // Simply rendering the area below the falling geometry dynamically
      // (i.e. rendering the actual true state of the map instead of the cached
      // VBOs) would not work, since we would then render the moving geometry
      // itself as well and that would destroy the smoothing.
      if (
        iFallingSectionInfo != fallingSections.end() &&
        index == iFallingSectionInfo->mIndex)
      {
        const auto& extraSection = iFallingSectionInfo->mSectionBelow;

        link.mExtraSection = MapGeometryLink::ExtraSection{
          engine::copyMapData(extraSection, *mpMap),
          extraSection.top(),
          extraSection.size.height};

        ++iFallingSectionInfo;
      }

      ++index;
    });
}


void DynamicGeometrySystem::receive(const events::ShootableKilled& event)
{
  auto entity = event.mEntity;
  // Take care of shootable walls
  if (!entity.has_component<MapGeometryLink>())
  {
    return;
  }

  const auto& mapSection =
    entity.component<MapGeometryLink>()->mLinkedGeometrySection;
  explodeMapSection(
    mapSection, *mpMap, *mpEntityManager, *mpEvents, *mpRandomGenerator);
  mpServiceProvider->playSound(data::SoundId::BigExplosion);
  mpEvents->emit(rigel::events::ScreenFlash{});
}


void DynamicGeometrySystem::receive(const rigel::events::DoorOpened& event)
{
  using namespace engine::components;
  using namespace game_logic::components;

  auto entity = event.mEntity;
  entity.remove<ActorTag>();
  entity.assign<ActivationSettings>(ActivationSettings::Policy::Always);
  entity.assign<BehaviorController>(behaviors::DynamicGeometryController{
    behaviors::DynamicGeometryController::Type::BlueKeyDoor});
}


void DynamicGeometrySystem::receive(
  const rigel::events::MissileDetonated& event)
{
  // TODO: Add a helper function for creating rectangles based on different
  // given values, e.g. bottom left and size
  engine::components::BoundingBox mapSection{
    event.mImpactPosition - base::Vec2{0, 2}, {3, 3}};
  explodeMapSection(
    mapSection, *mpMap, *mpEntityManager, *mpEvents, *mpRandomGenerator);
  mpEvents->emit(rigel::events::ScreenFlash{});
}


void DynamicGeometrySystem::receive(const rigel::events::TileBurnedAway& event)
{
  const auto [x, y] = event.mPosition;
  mpMap->setTileAt(0, x, y, 0);
  mpMap->setTileAt(1, x, y, 0);
}


void DynamicGeometrySystem::renderDynamicBackgroundSections(
  const base::Vec2& sectionStart,
  const base::Extents& sectionSize,
  const float interpolationFactor)
{
  renderDynamicSections(
    sectionStart,
    sectionSize,
    interpolationFactor,
    engine::MapRenderer::DrawMode::Background);
}


void DynamicGeometrySystem::renderDynamicForegroundSections(
  const base::Vec2& sectionStart,
  const base::Extents& sectionSize,
  const float interpolationFactor)
{
  renderDynamicSections(
    sectionStart,
    sectionSize,
    interpolationFactor,
    engine::MapRenderer::DrawMode::Foreground);
}


void DynamicGeometrySystem::renderDynamicSections(
  const base::Vec2& sectionStart,
  const base::Extents& sectionSize,
  const float interpolationFactor,
  const engine::MapRenderer::DrawMode drawMode)
{
  using engine::components::WorldPosition;

  const auto screenRect = base::Rect<int>{sectionStart, sectionSize};

  // Simple dynamic sections (burning tiles or destroyed by missile)
  for (const auto& section : mSimpleDynamicSections)
  {
    if (!screenRect.intersects(section))
    {
      continue;
    }

    const auto pixelPos =
      data::tileVectorToPixelVector(section.topLeft - sectionStart);

    mpMapRenderer->renderDynamicSection(*mpMap, section, pixelPos, drawMode);
  }

  // Falling dynamic geometry
  mpEntityManager->each<MapGeometryLink, WorldPosition>(
    [&](entityx::Entity e, const MapGeometryLink& link, const WorldPosition&) {
      // Render the geometry with smoothing, to make falling pieces of the map
      // appear smooth.
      if (screenRect.intersects(link.mLinkedGeometrySection))
      {
        const auto pixelPos =
          engine::interpolatedPixelPosition(e, interpolationFactor) -
          data::tileVectorToPixelVector(
            base::Vec2{0, link.mLinkedGeometrySection.size.height - 1} +
            sectionStart);
        mpMapRenderer->renderDynamicSection(
          *mpMap, link.mLinkedGeometrySection, pixelPos, drawMode);
      }

      // If there are non-zero tiles below the falling piece of geometry, we
      // also need to render them separately since these tiles disappear as the
      // piece of geometry is falling down. Also see comment in
      // initializeDynamicGeometryEntities().
      if (const auto extraSectionRect = link.extraSectionRect();
          extraSectionRect && extraSectionRect->size.height > 0 &&
          screenRect.intersects(*extraSectionRect))
      {
        const auto numSkippedRows =
          link.mExtraSection->mHeight - extraSectionRect->size.height;
        const auto numSkippedTiles =
          numSkippedRows * extraSectionRect->size.width;
        const auto mapDataView = base::ArrayView<uint32_t>{
          link.mExtraSection->mMapData.data() + numSkippedTiles,
          base::ArrayView<uint32_t>::size_type(
            link.mExtraSection->mMapData.size() - numSkippedTiles)};
        mpMapRenderer->renderCachedSection(
          data::tileVectorToPixelVector(
            extraSectionRect->topLeft - sectionStart),
          mapDataView,
          extraSectionRect->size.width,
          drawMode);
      }
    });
}


void behaviors::DynamicGeometryController::update(
  GlobalDependencies& d,
  GlobalState& s,
  const bool isOnScreen,
  entityx::Entity entity)
{
  using namespace engine::components;

  auto& position = *entity.component<WorldPosition>();
  auto& link = *entity.component<MapGeometryLink>();
  auto& mapSection = link.mLinkedGeometrySection;

  auto isOnSolidGround = [&]() {
    if (mapSection.bottom() >= s.mpMap->height() - 1)
    {
      return true;
    }

    const auto bottomLeft =
      s.mpMap->collisionData(mapSection.left(), mapSection.bottom() + 1);
    const auto bottomRight =
      s.mpMap->collisionData(mapSection.right(), mapSection.bottom() + 1);
    return bottomLeft.isSolidOn(data::map::SolidEdge::top()) ||
      bottomRight.isSolidOn(data::map::SolidEdge::top());
  };

  auto makeAlwaysActive = [&]() {
    engine::reassign<ActivationSettings>(
      entity, ActivationSettings::Policy::Always);
  };

  auto updateWaiting = [&](const int numFrames) {
    ++mFramesElapsed;
    if (mFramesElapsed == numFrames)
    {
      d.mpServiceProvider->playSound(data::SoundId::FallingRock);
    }
    else if (mFramesElapsed == numFrames + 1)
    {
      makeAlwaysActive();
      mState = State::Falling;
    }
  };

  auto fall = [&]() {
    for (int i = 0; i < GEOMETRY_FALL_SPEED; ++i)
    {
      if (isOnSolidGround())
      {
        return true;
      }

      moveTileSection(mapSection, *s.mpMap);
      ++position.y;
    }

    return false;
  };

  auto doBurnEffect = [&]() {
    d.mpEvents->emit(rigel::events::ScreenShake{2});
    d.mpServiceProvider->playSound(data::SoundId::HammerSmash);

    const auto offset = d.mpRandomGenerator->gen() % mapSection.size.width;
    const auto spawnPosition =
      base::Vec2{mapSection.left() + offset, mapSection.bottom() + 1};
    spawnFloatingOneShotSprite(
      *d.mpEntityFactory, data::ActorID::Shot_impact_FX, spawnPosition);
  };

  auto sink = [&]() {
    if (mapSection.size.height == 1)
    {
      s.mpMap->clearSection(
        mapSection.topLeft.x, mapSection.topLeft.y, mapSection.size.width, 1);
      d.mpServiceProvider->playSound(data::SoundId::BlueKeyDoorOpened);
      entity.destroy();
    }
    else
    {
      squashTileSection(mapSection, *s.mpMap);
    }
  };

  auto land = [&]() {
    d.mpServiceProvider->playSound(data::SoundId::BlueKeyDoorOpened);
    d.mpEvents->emit(rigel::events::ScreenShake{7});
  };

  auto explode = [&]() {
    explodeMapSection(mapSection, d, s);
    d.mpServiceProvider->playSound(data::SoundId::BigExplosion);
    entity.destroy();
  };


  auto updateType1 = [&, this]() {
    switch (mState)
    {
      case State::Waiting:
        updateWaiting(20);
        break;

      case State::Falling:
        if (fall())
        {
          mState = State::Sinking;
          doBurnEffect();
          sink();
        }
        break;

      case State::Sinking:
        doBurnEffect();
        sink();
        break;

      default:
        break;
    }
  };


  auto updateType2 = [&, this]() {
    switch (mState)
    {
      case State::Waiting:
        if (s.mpPerFrameState->mIsEarthShaking)
        {
          updateWaiting(2);
        }
        break;

      case State::Falling:
        if (fall())
        {
          explode();
        }
        break;

      default:
        break;
    }
  };


  auto updateType3 = [&, this]() {
    switch (mState)
    {
      case State::Waiting:
        if (!isOnSolidGround())
        {
          makeAlwaysActive();
          mState = State::Falling;
          if (fall())
          {
            land();
            mState = State::Waiting;
          }
        }
        break;

      case State::Falling:
        if (fall())
        {
          land();
          mState = State::Waiting;
        }
        break;

      default:
        break;
    }
  };


  auto updateType5 = [&, this]() {
    switch (mState)
    {
      case State::Waiting:
        if (!isOnSolidGround())
        {
          makeAlwaysActive();
          mState = State::Falling;
          if (fall())
          {
            explode();
          }
        }
        break;

      case State::Falling:
        if (fall())
        {
          explode();
        }
        break;

      default:
        break;
    }
  };


  auto updateType6 = [&, this]() {
    switch (mState)
    {
      case State::Waiting:
        updateWaiting(20);
        break;

      case State::Falling:
        for (int i = 0; i < GEOMETRY_FALL_SPEED; ++i)
        {
          if (isOnSolidGround())
          {
            land();
            mType = Type::FallDownImmediatelyThenStayOnGround;
            mState = State::Waiting;
          }
          else
          {
            moveTileSection(mapSection, *s.mpMap);
            ++position.y;
          }
        }
        break;

      default:
        break;
    }
  };


  auto updateDoor = [&]() {
    switch (mState)
    {
      case State::Waiting:
        ++mFramesElapsed;
        if (mFramesElapsed == 2)
        {
          makeAlwaysActive();
          mState = State::Falling;
        }
        break;

      case State::Falling:
        if (fall())
        {
          mState = State::Sinking;
          sink();
        }
        break;

      case State::Sinking:
        sink();
        break;

      default:
        break;
    }
  };


  switch (mType)
  {
    case Type::FallDownAfterDelayThenSinkIntoGround:
      updateType1();
      break;

    case Type::BlueKeyDoor:
      updateDoor();
      break;

    case Type::FallDownWhileEarthQuakeActiveThenExplode:
      updateType2();
      break;

    case Type::FallDownImmediatelyThenStayOnGround:
      updateType3();
      break;

    case Type::FallDownImmediatelyThenExplode:
      updateType5();
      break;

    case Type::FallDownAfterDelayThenStayOnGround:
      updateType6();
      break;

    default:
      break;
  }
}

} // namespace rigel::game_logic
