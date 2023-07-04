#pragma once
#include "src/Core/Core.h"
#include "src/Core/Maths.h"
#include "src/Core/Physics/PhysicsShapes.h"
#include "src/Scene/Entities/Entity.h"

namespace Nuake
{
	class Entity;

	namespace Physics
	{
		class CharacterController
		{
		public:
			
			Vector3 Position = Vector3(0, 0, 0);
			Entity Owner;

			bool IsOnGround = false;
			bool m_hittingWall;

			float m_stepHeight = 0.35f;
			float MaxSlopeAngle = 45.0f;
			float Friction = 0.5f;
			Ref<PhysicShape> Shape;

			//bool m_onJumpableGround; // A bit lower contact than just onGround

			float m_bottomYOffset;
			float m_bottomRoundedRegionYOffset;

			glm::vec3 m_manualVelocity;
			std::vector<glm::vec3> m_surfaceHitNormals;

			CharacterController(const Ref<PhysicShape>& shape, float friction, float maxSlopeAngle);

			void SetEntity(Entity& ent);
			Entity GetEntity() const;
			void MoveAndSlide(const Vector3& velocity);

			bool IsOnFloor()
			{
				return IsOnGround;
			}

		private:
			void ParseGhostContacts();
			void UpdatePosition();
			void UpdateVelocity();
		};
	}
}
