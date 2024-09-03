#include "SceneRenderer.h"
#include "src/Rendering/Shaders/ShaderManager.h"

#include "src/Scene/Components/BSPBrushComponent.h"
#include "src/Scene/Components/ModelComponent.h"
#include "src/Scene/Components/ParentComponent.h"
#include "src/Scene/Components/ParticleEmitterComponent.h"
#include "src/Scene/Components/SpriteComponent.h"

#include <glad/glad.h>
#include <src/Scene/Components/SkinnedModelComponent.h>
#include <src/Vendors/imgui/imgui.h>
#include <Tracy.hpp>


namespace Nuake 
{
	void SceneRenderer::Init()
	{
		const auto defaultResolution = Vector2(1920, 1080);
		mGBuffer = CreateScope<FrameBuffer>(false, defaultResolution);
		mGBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_DEPTH_COMPONENT), GL_DEPTH_ATTACHMENT); // Depth
		mGBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT0); // Albedo
		mGBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT1); // Normal
		mGBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGBA), GL_COLOR_ATTACHMENT2); // Material + unlit

		auto entityTexture = CreateRef<Texture>(defaultResolution, GL_RED_INTEGER, GL_R32I, GL_INT);
		entityTexture->SetParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		mGBuffer->SetTexture(entityTexture, GL_COLOR_ATTACHMENT3); // Entity ID
		mGBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RED, GL_R16F, GL_FLOAT), GL_COLOR_ATTACHMENT4); // Emissive

		mShadingBuffer = CreateScope<FrameBuffer>(true, defaultResolution);
		mShadingBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB, GL_RGB16F, GL_FLOAT));
		mShadingBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_DEPTH_COMPONENT), GL_DEPTH_ATTACHMENT); // Depth

		mToneMapBuffer = CreateScope<FrameBuffer>(false, defaultResolution);
		mToneMapBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT0);

		mBarrelDistortionBuffer = CreateScope<FrameBuffer>(false, defaultResolution);
		mBarrelDistortionBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT0);

		mVignetteBuffer = CreateScope<FrameBuffer>(false, defaultResolution);
		mVignetteBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT0);

		mDOFBuffer = CreateScope<FrameBuffer>(false, defaultResolution);
		mDOFBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT0);

		mOutlineBuffer = CreateScope<FrameBuffer>(false, defaultResolution);
		mOutlineBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT0);

		mDisplayDepthBuffer = CreateScope<FrameBuffer>(false, defaultResolution);
		mDisplayDepthBuffer->SetTexture(CreateRef<Texture>(defaultResolution, GL_RGB), GL_COLOR_ATTACHMENT0);

		// Generate debug meshes
		std::vector<Vertex> lineVertices
		{
			{ Vector3(0, 0, 0), Vector2(0, 0), Vector3(0, 0, 0) },
			{ Vector3(1, 1, 1), Vector2(0, 0), Vector3(0, 0, 0) }
		};

		std::vector<uint32_t> lineIndices
		{
			0, 1
		};

		// Debug shapes
		mLineMesh = CreateRef<Mesh>();
		mLineMesh->AddSurface(lineVertices, lineIndices);

		mBoxGizmo = CreateRef<BoxGizmo>();
		mBoxGizmo->CreateMesh();

		mSphereGizmo = CreateRef<SphereGizmo>();
		mSphereGizmo->CreateMesh();

		mCylinderGizmo = CreateRef<CylinderGizmo>();
		mCylinderGizmo->CreateMesh();

		mCapsuleGizmo = CreateRef<CapsuleGizmo>();
		mCapsuleGizmo->CreateMesh();

		mDebugLines = std::vector<DebugLine>();
		mDebugShapes = std::vector<DebugShape>();
	}

	void SceneRenderer::Cleanup()
	{
	}

	void SceneRenderer::Update(const Timestep time, bool isEditorUpdate)
	{
		// Delete debug shapes that are dead
		if (mDebugLines.size() > 0)
		{
			std::erase_if(mDebugLines, [](const DebugLine& line)
				{
					return line.Life < 0.0f;
				});

			for (auto& line : mDebugLines)
			{
				line.Life -= time;
			}
		}

		if (mDebugShapes.size() > 0)
		{
			std::erase_if(mDebugShapes, [](const DebugShape& shape)
				{
					return shape.Life < 0.0f;
				});

			for (auto& shape : mDebugShapes)
			{
				shape.Life -= time;
			}
		}
	}

	void SceneRenderer::BeginRenderScene(const Matrix4& projection, const Matrix4& view, const Vector3& camPos)
	{
		mProjection = projection;
		mView = view;
		mCamPos = camPos;
	}

	/// <summary>
	/// Renders a scene to a framebuffer. The size of the framebuffer will be used.
	/// </summary>
	/// <param name="scene">Scene to render</param>
	/// <param name="framebuffer">Framebuffer to render the scene to. Should be in the right size</param>
	void SceneRenderer::RenderScene(Scene& scene, FrameBuffer& framebuffer)
	{
		ZoneScoped;

		// Renders all shadow maps
		ShadowPass(scene);

		const Vector2 framebufferResolution = framebuffer.GetSize();
		mGBuffer->QueueResize(framebufferResolution);
		GBufferPass(scene);

		// SSAO
		const auto& sceneEnv = scene.GetEnvironment();
		if (sceneEnv->SSAOEnabled)
		{
			sceneEnv->mSSAO->Resize(framebuffer.GetSize());
			sceneEnv->mSSAO->Draw(mGBuffer.get(), mProjection, mView);
		}
		else
		{
			sceneEnv->mSSAO->Clear();
		}

		mShadingBuffer->QueueResize(framebufferResolution);
		ShadingPass(scene);

		// Blit depth buffer
		glBindFramebuffer(GL_READ_FRAMEBUFFER, mGBuffer->GetRenderID());
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mShadingBuffer->GetRenderID());

		glBlitFramebuffer(
			0, 0, mShadingBuffer->GetSize().x, mShadingBuffer->GetSize().y, // Source rectangle (x0, y0, x1, y1)
			0, 0, mShadingBuffer->GetSize().x, mShadingBuffer->GetSize().y, // Destination rectangle (x0, y0, x1, y1)
			GL_DEPTH_BUFFER_BIT, // Bitmask indicating which buffers to copy
			GL_NEAREST // Filtering mode (NEAREST or LINEAR)
		);

		DebugRendererPass(scene);

		Ref<Texture> finalOutput = mShadingBuffer->GetTexture();
		if (scene.GetEnvironment()->BloomEnabled)
		{
			sceneEnv->mBloom->SetSource(mShadingBuffer->GetTexture());
			sceneEnv->mBloom->Resize(framebuffer.GetSize());
			sceneEnv->mBloom->Draw();

			finalOutput = sceneEnv->mBloom->GetOutput();
		}

		const auto view = scene.m_Registry.view<LightComponent>();
		auto lightList = std::vector<LightComponent>();
		for (auto l : view)
		{
			auto& lc = view.get<LightComponent>(l);
			if (lc.Type == Directional && lc.IsVolumetric && lc.CastShadows)
				lightList.push_back(lc);
		}
		
		glDepthMask(false);
		if (sceneEnv->VolumetricEnabled)
		{
			sceneEnv->mVolumetric->Resize(framebuffer.GetSize());
			sceneEnv->mVolumetric->SetDepth(mGBuffer->GetTexture(GL_DEPTH_ATTACHMENT).get());
			sceneEnv->mVolumetric->Draw(mProjection, mView, mCamPos, lightList);

			//finalOutput = mVolumetric->GetFinalOutput().get();

			// combine
			
			framebuffer.Bind();
			{
				RenderCommand::Clear();
				Shader* shader = ShaderManager::GetShader("Resources/Shaders/combine.shader");
				shader->Bind();

				shader->SetUniformTex("u_Source", finalOutput.get(), 0);
				shader->SetUniformTex("u_Source2", sceneEnv->mVolumetric->GetFinalOutput().get(), 1);
				Renderer::DrawQuad();
			}
			framebuffer.Unbind();
		}
		else
		{
			framebuffer.Bind();
			{
				RenderCommand::Clear();
				Shader* shader = ShaderManager::GetShader("Resources/Shaders/copy.shader");
				shader->Bind();

				shader->SetUniformTex("u_Source", finalOutput.get(), 0);
				Renderer::DrawQuad();
			}
		}

		finalOutput = framebuffer.GetTexture();

		// Copy final output to target framebuffer
		mToneMapBuffer->QueueResize(framebufferResolution);
		mToneMapBuffer->Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/tonemap.shader");
			shader->Bind();

			shader->SetUniform1f("u_Exposure", sceneEnv->Exposure);
			shader->SetUniform1f("u_Gamma", sceneEnv->Gamma);
			shader->SetUniformTex("u_Source", finalOutput.get());
			Renderer::DrawQuad();
		}
		mToneMapBuffer->Unbind();

		ProjectSettings projectSettings = Engine::GetProject()->Settings;

		mOutlineBuffer->QueueResize(framebufferResolution);
		mOutlineBuffer->Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/outline.shader");
			shader->Bind();

			shader->SetUniform1i("u_EntityID", mOutlineEntityID == -1 ? -1 : mOutlineEntityID + 1);
			shader->SetUniformTex("u_EntityTexture", mGBuffer->GetTexture(GL_COLOR_ATTACHMENT3).get(), 0);
			shader->SetUniformVec4("u_OutlineColor", projectSettings.PrimaryColor);
			shader->SetUniformTex("u_Depth", mGBuffer->GetTexture(GL_DEPTH_ATTACHMENT), 1);
			shader->SetUniform1f("u_Radius", projectSettings.OutlineRadius * projectSettings.ResolutionScale);
			Renderer::DrawQuad();
		}
		mOutlineBuffer->Unbind();

		framebuffer.Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/add.shader");
			shader->Bind();

			shader->SetUniformTex("u_Source", mToneMapBuffer->GetTexture().get(), 0);
			shader->SetUniformTex("u_Source2", mOutlineBuffer->GetTexture().get(), 1);
			Renderer::DrawQuad();
		}
		framebuffer.Unbind();

		mToneMapBuffer->Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/copy.shader");
			shader->Bind();

			shader->SetUniformTex("u_Source", framebuffer.GetTexture().get(), 1);
			Renderer::DrawQuad();
		}
		mToneMapBuffer->Unbind();

		if (sceneEnv->SSREnabled)
		{
			sceneEnv->mSSR->Resize(framebufferResolution);
			sceneEnv->mSSR->Draw(mGBuffer.get(), framebuffer.GetTexture(), mView, mProjection, scene.GetCurrentCamera());

			framebuffer.Bind();
			{
				RenderCommand::Clear();
				Shader* shader = ShaderManager::GetShader("Resources/Shaders/combine.shader");
				shader->Bind();

				shader->SetUniformTex("u_Source", mToneMapBuffer->GetTexture().get(), 0);
				shader->SetUniformTex("u_Source2", sceneEnv->mSSR->OutputFramebuffer->GetTexture().get(), 1);
				Renderer::DrawQuad();
			}
			framebuffer.Unbind();
		}
		else
		{
			framebuffer.Bind();
			{
				RenderCommand::Clear();
				Shader* shader = ShaderManager::GetShader("Resources/Shaders/copy.shader");
				shader->Bind();

				shader->SetUniformTex("u_Source", mToneMapBuffer->GetTexture().get(), 0);
				Renderer::DrawQuad();
			}
			framebuffer.Unbind();
		}

		mDOFBuffer->QueueResize(framebufferResolution);
		mDOFBuffer->Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/dof.shader");
			shader->Bind();

			shader->SetUniform1f("focalDepth", sceneEnv->DOFFocalDepth);
			shader->SetUniform1f("focalLength", sceneEnv->DOFFocalLength);
			shader->SetUniform1f("fstop", sceneEnv->DOFFstop);
			shader->SetUniform1i("showFocus", sceneEnv->DOFShowFocus);
			shader->SetUniform1i("autofocus", sceneEnv->DOFAutoFocus);
			shader->SetUniform1i("samples", sceneEnv->DOFSamples);
			shader->SetUniform1i("manualdof", sceneEnv->DOFManualFocus);
			shader->SetUniform1f("rings", static_cast<float>(sceneEnv->DOFrings));
			shader->SetUniform1f("ndofstart", sceneEnv->DOFStart);
			shader->SetUniform1f("ndofdist", sceneEnv->DOFDist);
			shader->SetUniform1f("fdofstart", sceneEnv->DOFStart);
			shader->SetUniform1f("fdofdist", sceneEnv->DOFDist);	
			shader->SetUniform1f("CoC", sceneEnv->DOFCoc);
			shader->SetUniform1f("maxblur", sceneEnv->DOFMaxBlue);
			shader->SetUniform1f("threshold", sceneEnv->DOFThreshold);
			shader->SetUniform1f("gain", sceneEnv->DOFGain);
			shader->SetUniform1f("bias", sceneEnv->DOFBias);
			shader->SetUniform1f("fringe", sceneEnv->DOFFringe);
			shader->SetUniform1f("namount", sceneEnv->DOFNAmmount);
			shader->SetUniform1f("dbsize", sceneEnv->DOFDbSize);
			shader->SetUniform1f("feather", sceneEnv->DOFFeather);
			shader->SetUniform1f("u_Distortion", sceneEnv->BarrelDistortion);
			shader->SetUniform1f("height", static_cast<float>(finalOutput->GetHeight()));
			shader->SetUniform1f("width", static_cast<float>(finalOutput->GetWidth()));
			shader->SetUniformTex("depthTex", mGBuffer->GetTexture(GL_DEPTH_ATTACHMENT).get(), 0);
			shader->SetUniformTex("renderTex", finalOutput.get(), 1);
			Renderer::DrawQuad();
		}
		mDOFBuffer->Unbind();

		if (sceneEnv->BarrelDistortionEnabled)
		{
			mBarrelDistortionBuffer->QueueResize(framebufferResolution);
			mBarrelDistortionBuffer->Bind();
			{
				RenderCommand::Clear();
				Shader* shader = ShaderManager::GetShader("Resources/Shaders/barrel_distortion.shader");
				shader->Bind();

				shader->SetUniform1f("u_Distortion", sceneEnv->BarrelDistortion);
				shader->SetUniform1f("u_DistortionEdge", sceneEnv->BarrelEdgeDistortion);
				shader->SetUniform1f("u_Scale", sceneEnv->BarrelScale);

				if (sceneEnv->DOFEnabled)
				{
					shader->SetUniformTex("u_Source", mDOFBuffer->GetTexture().get(), 0);
				}
				else
				{
					shader->SetUniformTex("u_Source", finalOutput.get(), 0);
				}

				
				Renderer::DrawQuad();
			}
			mBarrelDistortionBuffer->Unbind();

			framebuffer.Bind();
			{
				RenderCommand::Clear();
				Shader* shader = ShaderManager::GetShader("Resources/Shaders/copy.shader");
				shader->Bind();

				shader->SetUniformTex("u_Source", mBarrelDistortionBuffer->GetTexture().get(), 0);
				Renderer::DrawQuad();
			}
			framebuffer.Unbind();
		}

		mVignetteBuffer->QueueResize(framebufferResolution);
		mVignetteBuffer->Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/vignette.shader");
			shader->Bind();

			shader->SetUniform1f("u_Intensity", sceneEnv->VignetteIntensity);
			shader->SetUniform1f("u_Extend", sceneEnv->VignetteEnabled ? sceneEnv->VignetteExtend : 0.0f);
			shader->SetUniformTex("u_Source", finalOutput.get(), 0);
			Renderer::DrawQuad();
		}
		mVignetteBuffer->Unbind();

		framebuffer.Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/add.shader");
			shader->Bind();

			shader->SetUniformTex("u_Source", finalOutput.get(), 0);
			shader->SetUniformTex("u_Source2", mOutlineBuffer->GetTexture().get(), 1);
			Renderer::DrawQuad();
		}
		framebuffer.Unbind();

		framebuffer.Bind();
		{
			RenderCommand::Clear();
			Shader* shader = ShaderManager::GetShader("Resources/Shaders/copy.shader");
			shader->Bind();

			shader->SetUniformTex("u_Source", mVignetteBuffer->GetTexture().get(), 0);
			Renderer::DrawQuad();
		}
		framebuffer.Unbind();

		glDepthMask(true);

		// Barrel distortion
		//mVignetteBuffer->Bind();
		//{
		//	RenderCommand::Clear();
		//	Shader* shader = ShaderManager::GetShader("Resources/Shaders/vignette.shader");
		//	shader->Bind();
		//
		//	shader->SetUniform1f("u_Intensity", sceneEnv->VignetteIntensity);
		//	shader->SetUniform1f("u_Extend", sceneEnv->VignetteExtend);
		//	shader->SetUniformTex("u_Source", mBarrelDistortionBuffer->GetTexture().get(), 0);
		//	Renderer::DrawQuad();
		//}
		//mVignetteBuffer->Unbind();


		RenderCommand::Enable(RendererEnum::DEPTH_TEST);
		Renderer::EndDraw();
	}

	void SceneRenderer::DrawTemporaryModel(const std::string & name, Ref<Model> model, Matrix4 transform)
	{
		if (IsTempModelLoaded(name))
		{
			mTempModels[name].Transform = transform;
			return;
		}

		mTempModels[name] = TemporaryModels{ model, transform };
	}

	void SceneRenderer::DrawDebugLine(const Vector3& start, const Vector3& end, const Color& color, float life, float width)
	{
		DebugLine debugLine = {
			.Start = start,
			.End = end,
			.LineColor = color,
			.Life = life,
			.Width = width,
			.DepthTest = true
		};

		mDebugLines.push_back(debugLine);
	}

	void SceneRenderer::DrawDebugShape(const Vector3& position, const Quat& rotation, Ref<Physics::PhysicShape> shape, const Color& color, float life, float width)
	{
		DebugShape debugShape = {
			.Position = position,
			.Rotation = rotation,
			.LineColor = color,
			.Life = life,
			.Width = width,
			.DepthTest = true,
			.Shape = shape,
		};

		mDebugShapes.push_back(debugShape);
	}
	
	void SceneRenderer::ShadowPass(Scene& scene)
	{
		ZoneScoped;

		RenderCommand::Enable(RendererEnum::DEPTH_TEST);

		Shader* shader = ShaderManager::GetShader("Resources/Shaders/shadowMap.shader");
		shader->Bind();

		RenderCommand::Enable(RendererEnum::FACE_CULL);
		glCullFace(GL_BACK);

		auto meshView = scene.m_Registry.view<TransformComponent, ModelComponent, VisibilityComponent>();
		auto quakeView = scene.m_Registry.view<TransformComponent, BSPBrushComponent, VisibilityComponent>();
		auto view = scene.m_Registry.view<TransformComponent, LightComponent, VisibilityComponent>();
		LightComponent lightDebug;

		for (auto l : view)
		{
			auto [lightTransform, light, visibility] = view.get<TransformComponent, LightComponent, VisibilityComponent>(l);
			if (!light.CastShadows || !visibility.Visible)
			{
				continue;
			}

			if (light.Type == LightType::Directional)
			{
				light.CalculateViewProjection(mView, mProjection);

				for (int i = 0; i < CSM_AMOUNT; i++)
				{
					light.m_Framebuffers[i]->Bind();
					light.m_Framebuffers[i]->Clear();
					{
						shader->SetUniformMat4f("u_LightTransform", light.mViewProjections[i]);
						for (auto e : meshView)
						{
							auto [transform, mesh, visibility] = meshView.get<TransformComponent, ModelComponent, VisibilityComponent>(e);
							if (mesh.ModelResource != nullptr && visibility.Visible)
							{
								for (auto& m : mesh.ModelResource->GetMeshes())
									Renderer::SubmitMesh(m, transform.GetGlobalTransform());
							}
						}

						for (auto e : quakeView)
						{
							auto [transform, model, visibility] = quakeView.get<TransformComponent, BSPBrushComponent, VisibilityComponent>(e);

							if (model.IsTransparent || !visibility.Visible)
								continue;

							
						}
						Renderer::Flush(shader, true);

						auto spriteView = scene.m_Registry.view<TransformComponent, SpriteComponent, VisibilityComponent>();
						for (auto e : spriteView)
						{
							auto [transform, sprite, visibility] = spriteView.get<TransformComponent, SpriteComponent, VisibilityComponent>(e);

							if (!visibility.Visible || !sprite.SpriteMesh)
								continue;

							auto finalQuadTransform = transform.GetGlobalTransform();
							if (sprite.Billboard)
							{
								if (sprite.PositionFacing)
								{
									const Matrix4& invView = glm::inverse(mView);
									const Vector3& cameraPosition = Vector3(invView[3][0], invView[3][1], invView[3][2]);
									const Vector3& spritePosition = Vector3(finalQuadTransform[3][0], finalQuadTransform[3][1], finalQuadTransform[3][2]);
									const Vector3& direction = cameraPosition - spritePosition;
									finalQuadTransform = glm::inverse(glm::lookAt(Vector3(), direction, Vector3(0, 1, 0)));
								}
								else
								{
									finalQuadTransform = glm::inverse(mView);

								}

								if (sprite.LockYRotation)
								{
									// This locks the pitch rotation on the billboard, useful for trees, lamps, etc.
									finalQuadTransform[1] = Vector4(0, 1, 0, 0);
									finalQuadTransform[2] = Vector4(finalQuadTransform[2][0], 0, finalQuadTransform[2][2], 0);
									finalQuadTransform = finalQuadTransform;
								}

								finalQuadTransform[3] = Vector4(Vector3(transform.GetGlobalTransform()[3]), 1.0f);

								// Scale
								finalQuadTransform = glm::scale(finalQuadTransform, transform.GetGlobalScale());
							}

							Renderer::SubmitMesh(sprite.SpriteMesh, finalQuadTransform, (uint32_t)e);
						}

						Renderer::Flush(shader, true);
					}
				}
			}
			else if (light.Type == LightType::Spot)
			{
				RenderCommand::Disable(RendererEnum::FACE_CULL);

				light.m_Framebuffers[0]->Bind();
				light.m_Framebuffers[0]->Clear();
				{
					Matrix4 spotLightTransform = Matrix4(1.0f);


					Vector3 pos = lightTransform.GetGlobalPosition();
					pos.y *= -1.0f;
					pos.x *= -1.0f;
					pos.z *= -1.0f;
					spotLightTransform = glm::translate(spotLightTransform, pos);

					Vector3 direction = lightTransform.GetGlobalRotation() * Vector3(0, 0, 1);
					auto lookatAt = lookAt(Vector3(), direction, Vector3(0, 1, 0));
					Quat offset = QuatFromEuler(0, -90.0f, 0);
					Quat offset2 = QuatFromEuler(180.0f, 0.0f, 0);
					
					const Quat& globalRotation = glm::normalize(lightTransform.GetGlobalRotation());
					const Matrix4& rotationMatrix = glm::mat4_cast(globalRotation);

					shader->SetUniformMat4f("u_LightTransform", light.GetProjection() * glm::inverse(lightTransform.GetGlobalTransform()));
					for (auto e : meshView)
					{
						auto [transform, mesh, visibility] = meshView.get<TransformComponent, ModelComponent, VisibilityComponent>(e);
						if (mesh.ModelResource != nullptr && visibility.Visible)
						{
							for (auto& m : mesh.ModelResource->GetMeshes())
								Renderer::SubmitMesh(m, transform.GetGlobalTransform());
						}
					}

					for (auto e : quakeView)
					{
						auto [transform, model, visibility] = quakeView.get<TransformComponent, BSPBrushComponent, VisibilityComponent>(e);

						if (model.IsTransparent || !visibility.Visible)
							continue;

						
					}
					Renderer::Flush(shader, true);

					auto spriteView = scene.m_Registry.view<TransformComponent, SpriteComponent, VisibilityComponent>();
					for (auto e : spriteView)
					{
						auto [transform, sprite, visibility] = spriteView.get<TransformComponent, SpriteComponent, VisibilityComponent>(e);

						if (!visibility.Visible || !sprite.SpriteMesh)
							continue;

						auto finalQuadTransform = transform.GetGlobalTransform();
						if (sprite.Billboard)
						{
							if (sprite.PositionFacing)
							{
								const Matrix4& invView = glm::inverse(mView);
								const Vector3& cameraPosition = Vector3(invView[3][0], invView[3][1], invView[3][2]);
								const Vector3& spritePosition = Vector3(finalQuadTransform[3][0], finalQuadTransform[3][1], finalQuadTransform[3][2]);
								const Vector3& direction = cameraPosition - spritePosition;
								finalQuadTransform = glm::inverse(glm::lookAt(Vector3(), direction, Vector3(0, 1, 0)));
							}
							else
							{
								finalQuadTransform = glm::inverse(mView);

							}

							if (sprite.LockYRotation)
							{
								// This locks the pitch rotation on the billboard, useful for trees, lamps, etc.
								finalQuadTransform[1] = Vector4(0, 1, 0, 0);
								finalQuadTransform[2] = Vector4(finalQuadTransform[2][0], 0, finalQuadTransform[2][2], 0);
								finalQuadTransform = finalQuadTransform;
							}

							finalQuadTransform[3] = Vector4(Vector3(transform.GetGlobalTransform()[3]), 1.0f);

							// Scale
							finalQuadTransform = glm::scale(finalQuadTransform, transform.GetGlobalScale());
						}

						Renderer::SubmitMesh(sprite.SpriteMesh, finalQuadTransform, (uint32_t)e);
					}

					Renderer::Flush(shader, true);
				}

				lightDebug = light;
			}
		}

		Shader* gBufferSkinnedMeshShader = ShaderManager::GetShader("Resources/Shaders/shadowMap_skinned.shader");
		gBufferSkinnedMeshShader->Bind();
		const uint32_t modelMatrixUniformLocation = gBufferSkinnedMeshShader->FindUniformLocation("u_Model");
		gBufferSkinnedMeshShader->SetUniformMat4f(modelMatrixUniformLocation, Matrix4(1.0f));
		
		auto skinnedView = scene.m_Registry.view<TransformComponent, SkinnedModelComponent, VisibilityComponent>();
		for (auto l : view)
		{
			auto [lightTransform, light, visibility] = view.get<TransformComponent, LightComponent, VisibilityComponent>(l);
			if (light.Type != LightType::Directional || !light.CastShadows || !visibility.Visible)
			{
				continue;
			}
		
			for (int i = 0; i < CSM_AMOUNT; i++)
			{
				light.m_Framebuffers[i]->Bind();
				{
					gBufferSkinnedMeshShader->SetUniformMat4f("u_LightTransform", light.mViewProjections[i]);
					for (auto e : skinnedView)
					{
						auto [transform, mesh, visibility] = skinnedView.get<TransformComponent, SkinnedModelComponent, VisibilityComponent>(e);
						if (mesh.ModelResource != nullptr && visibility.Visible)
						{
							auto& rootBoneNode = mesh.ModelResource->GetSkeletonRootNode();
							SetSkeletonBoneTransformRecursive(scene, rootBoneNode, gBufferSkinnedMeshShader);

							for (auto& m : mesh.ModelResource->GetMeshes())
							{
								m->Draw(gBufferSkinnedMeshShader, false);
							}
						}
					}
				}
			}
		}

		mDisplayDepthBuffer->QueueResize(GetGBuffer().GetTexture(GL_DEPTH_ATTACHMENT)->GetSize());

		mDisplayDepthBuffer->Bind();
		mDisplayDepthBuffer->Clear();
		Shader* displayDepthShader = ShaderManager::GetShader("Resources/Shaders/display_depth.shader");
		displayDepthShader->Bind();

		GetGBuffer().GetTexture(GL_DEPTH_ATTACHMENT)->Bind(5);
		displayDepthShader->SetUniform1i("u_Source", 5);

		RenderCommand::Disable(RendererEnum::DEPTH_TEST);
		Renderer::DrawQuad(Matrix4(1.0f));
		RenderCommand::Enable(RendererEnum::DEPTH_TEST);
		mDisplayDepthBuffer->Unbind();
	}

	void SceneRenderer::GBufferPass(Scene& scene)
	{
		ZoneScoped;

		mGBuffer->Bind();
		mGBuffer->Clear();
		{
			RenderCommand::Disable(RendererEnum::BLENDING);

			// Init
			RenderCommand::Disable(RendererEnum::FACE_CULL);
			Shader* gBufferShader = ShaderManager::GetShader("Resources/Shaders/gbuffer.shader");
			Shader* gBufferSkinnedMeshShader = ShaderManager::GetShader("Resources/Shaders/gbuffer_skinned.shader");

			gBufferShader->Bind();
			gBufferShader->SetUniformMat4f("u_Projection", mProjection);
			gBufferShader->SetUniformMat4f("u_View", mView);

			// Models
			auto view = scene.m_Registry.view<TransformComponent, ModelComponent, VisibilityComponent>();
			for (auto e : view)
			{
				auto [transform, mesh, visibility] = view.get<TransformComponent, ModelComponent, VisibilityComponent>(e);
				
				if (mesh.ModelResource && visibility.Visible)
				{
					for (auto& m : mesh.ModelResource->GetMeshes())
					{
						Renderer::SubmitMesh(m, transform.GetGlobalTransform(), (uint32_t)e);
					}
				}
			}
			Renderer::Flush(gBufferShader, false);

			for (auto& mesh : mTempModels)
			{
				if (mesh.second.ModelResource)
				{
					for (auto& m : mesh.second.ModelResource->GetMeshes())
					{
						Renderer::SubmitMesh(m, mesh.second.Transform, (uint32_t)-1);
					}
				}
			}

			glEnable(GL_DEPTH_TEST);
			glDepthMask(GL_FALSE);
			Renderer::Flush(gBufferShader, true);
			glDepthMask(GL_TRUE);

			// Quake BSPs
			auto quakeView = scene.m_Registry.view<TransformComponent, BSPBrushComponent, VisibilityComponent>();
			for (auto e : quakeView)
			{
				auto [transform, model, visibility] = quakeView.get<TransformComponent, BSPBrushComponent, VisibilityComponent>(e);

				if (model.IsTransparent || !visibility.Visible)
					continue;

				
			}
			glEnable(GL_DEPTH_TEST);
			glDisable(GL_CULL_FACE);
			glCullFace(GL_FRONT);
			Renderer::Flush(gBufferShader, true);

			// Sprites
			auto spriteView = scene.m_Registry.view<TransformComponent, SpriteComponent, VisibilityComponent>();
			for (auto& e : spriteView)
			{
				auto [transform, sprite, visibility] = spriteView.get<TransformComponent, SpriteComponent, VisibilityComponent>(e);

				if (!visibility.Visible || !sprite.SpriteMesh)
					continue;

				auto finalQuadTransform = transform.GetGlobalTransform();
				if (sprite.Billboard)
				{
					if (sprite.PositionFacing)
					{
						const Matrix4& invView = glm::inverse(mView);
						const Vector3& cameraPosition = Vector3(invView[3][0], invView[3][1], invView[3][2]);
						const Vector3& spritePosition = Vector3(finalQuadTransform[3][0], finalQuadTransform[3][1], finalQuadTransform[3][2]);
						const Vector3& direction = cameraPosition - spritePosition;
						finalQuadTransform = glm::inverse(glm::lookAt(Vector3(), direction, Vector3(0, 1, 0)));
					}
					else
					{

						finalQuadTransform = glm::inverse(mView);
						
					}

					if (sprite.LockYRotation)
					{
						// This locks the pitch rotation on the billboard, useful for trees, lamps, etc.
						finalQuadTransform[1] = Vector4(0, 1, 0, 0);
						finalQuadTransform[2] = Vector4(finalQuadTransform[2][0], 0, finalQuadTransform[2][2], 0);
						finalQuadTransform = finalQuadTransform;
					}

					finalQuadTransform[3] = Vector4(Vector3(transform.GetGlobalTransform()[3]), 1.0f);

					// Scale
					finalQuadTransform = glm::scale(finalQuadTransform, transform.GetGlobalScale());
				}

				Renderer::SubmitMesh(sprite.SpriteMesh, finalQuadTransform, (uint32_t)e);
			}
			Renderer::Flush(gBufferShader, false);

			// Particles
			Ref<Material> previousMaterial = Renderer::QuadMesh->GetMaterial();

			auto particleEmitterView = scene.m_Registry.view<TransformComponent, ParticleEmitterComponent, VisibilityComponent>();
			for (auto& e : particleEmitterView)
			{
				auto [transform, emitterComponent, visibility] = particleEmitterView.get<TransformComponent, ParticleEmitterComponent, VisibilityComponent>(e);

				if (!visibility.Visible || !emitterComponent.ParticleMaterial)
					continue;

				Renderer::QuadMesh->SetMaterial(emitterComponent.ParticleMaterial);

				Vector3 oldColor = Renderer::QuadMesh->GetMaterial()->data.m_AlbedoColor;
				auto initialTransform = transform.GetGlobalTransform();
				for (auto& p : emitterComponent.Emitter.Particles)
				{
					Matrix4 particleTransform = initialTransform;
					particleTransform = glm::inverse(mView);

					// Translation
					Vector3 particleGlobalPosition;
					if (emitterComponent.GlobalSpace)
					{
						particleGlobalPosition = p.Position;
					}
					else
					{
						particleGlobalPosition = Vector3(initialTransform[3]) + p.Position;
					}
					particleTransform[3] = Vector4(particleGlobalPosition, 1.0f);

					// Scale
					Vector3 finalScale = emitterComponent.ParticleScale;
					if (p.Scale != 1.0f)
					{
						finalScale += emitterComponent.ParticleScale * p.Scale;
					}

					particleTransform = glm::scale(particleTransform, finalScale);
					
					Renderer::SubmitMesh(Renderer::QuadMesh, particleTransform, (uint32_t)e);
				}

				Renderer::QuadMesh->SetMaterial(emitterComponent.ParticleMaterial);
				Renderer::Flush(gBufferShader, false);

				Renderer::QuadMesh->SetMaterial(previousMaterial);
			}


			// Temp models




			// Reset material on quadmesh
			// Renderer::QuadMesh->SetMaterial(previousMaterial);

			// Skinned mesh at the end because we switch shader
			gBufferSkinnedMeshShader->Bind();
			gBufferSkinnedMeshShader->SetUniformMat4f("u_Projection", mProjection);
			gBufferSkinnedMeshShader->SetUniformMat4f("u_View", mView);

			RenderCommand::Disable(RendererEnum::FACE_CULL);

			// Skinned Models
			const uint32_t entityIdUniformLocation = gBufferSkinnedMeshShader->FindUniformLocation("u_EntityID");
			const uint32_t modelMatrixUniformLocation = gBufferSkinnedMeshShader->FindUniformLocation("u_Model");
			gBufferSkinnedMeshShader->SetUniformMat4f(modelMatrixUniformLocation, Matrix4(1.0f));
			auto skinnedModelView = scene.m_Registry.view<TransformComponent, SkinnedModelComponent, VisibilityComponent>();
			for (auto e : skinnedModelView)
			{
				auto [transform, mesh, visibility] = skinnedModelView.get<TransformComponent, SkinnedModelComponent, VisibilityComponent>(e);
				auto& meshResource = mesh.ModelResource;

				if (meshResource && visibility.Visible)
				{
					auto& rootBoneNode = meshResource->GetSkeletonRootNode();
					SetSkeletonBoneTransformRecursive(scene, rootBoneNode, gBufferSkinnedMeshShader);

					for (auto& m : mesh.ModelResource->GetMeshes())
					{
						m->GetMaterial()->Bind(gBufferSkinnedMeshShader);
						
						gBufferSkinnedMeshShader->SetUniform1i(entityIdUniformLocation, (uint32_t)e + 1);
						m->Draw(gBufferSkinnedMeshShader, true);
					}
				}
			}
		}

		RenderCommand::Enable(RendererEnum::BLENDING);
	}

	void SceneRenderer::ShadingPass(Scene& scene)
	{
		ZoneScoped;

		mShadingBuffer->Bind();
		mShadingBuffer->Clear();
		{
			RenderCommand::Disable(RendererEnum::DEPTH_TEST);
			RenderCommand::Disable(RendererEnum::FACE_CULL);
			Ref<Environment> environment = scene.GetEnvironment();
			if (environment->CurrentSkyType == SkyType::ProceduralSky)
			{
				RenderCommand::Clear();
				RenderCommand::SetClearColor(Color(0, 0, 0, 1));
				environment->ProceduralSkybox->Draw(mProjection, mView);
			}
			else if (environment->CurrentSkyType == SkyType::ClearColor)
			{
				RenderCommand::SetClearColor(environment->AmbientColor);
				RenderCommand::Clear();
				RenderCommand::SetClearColor(Color(0, 0, 0, 1));
			}

			RenderCommand::Enable(RendererEnum::FACE_CULL);

			Shader* shadingShader = ShaderManager::GetShader("Resources/Shaders/deferred.shader");
			shadingShader->Bind();
			shadingShader->SetUniformMat4f("u_Projection", mProjection);
			shadingShader->SetUniformMat4f("u_View", mView);
			shadingShader->SetUniformVec3("u_EyePosition", scene.GetCurrentCamera()->Translation);
			shadingShader->SetUniform1f("u_AmbientTerm", environment->AmbientTerm);
			shadingShader->SetUniformTex("m_SSAO", scene.GetEnvironment()->mSSAO->GetOuput()->GetTexture().get(), 9);

			Ref<Environment> env = scene.GetEnvironment();

			struct LightDistance
			{
				TransformComponent transform;
				LightComponent light;
				float distance;
			};
			std::vector<LightDistance> lightDistances;

			auto view = scene.m_Registry.view<TransformComponent, LightComponent, ParentComponent>();

			lightDistances.reserve(view.size_hint());

			const Vector3 camPosition = scene.GetCurrentCamera()->Translation;
			for (auto l : view)
			{
				auto [transform, light, parent] = view.get<TransformComponent, LightComponent, ParentComponent>(l);

				if (light.Type == Directional)
				{
					if (light.SyncDirectionWithSky)
					{
						light.Direction = env->ProceduralSkybox->GetSunDirection();
					}
					else
					{
						light.Direction = transform.GetGlobalRotation() * Vector3(0, 0, 1);
					}
				}

				Vector3 lightPosition = transform.GetGlobalPosition();
				float distanceFromCam = glm::length(camPosition - lightPosition);

				lightDistances.push_back({transform, light, distanceFromCam});
			}

			std::sort(lightDistances.begin(), lightDistances.end(), 
				[](const LightDistance& a, const LightDistance& b) 
				{
					return a.distance < b.distance;
				}
			);

			for (const auto& l : lightDistances)
			{
				Renderer::RegisterDeferredLight(l.transform, l.light);
			}

			mGBuffer->GetTexture(GL_DEPTH_ATTACHMENT)->Bind(5);
			mGBuffer->GetTexture(GL_COLOR_ATTACHMENT0)->Bind(6);
			mGBuffer->GetTexture(GL_COLOR_ATTACHMENT1)->Bind(7);
			mGBuffer->GetTexture(GL_COLOR_ATTACHMENT2)->Bind(8);
			mGBuffer->GetTexture(GL_COLOR_ATTACHMENT4)->Bind(10);

			shadingShader->SetUniform1i("m_Depth", 5);
			shadingShader->SetUniform1i("m_Albedo", 6);
			shadingShader->SetUniform1i("m_Normal", 7);
			shadingShader->SetUniform1i("m_Material", 8);
			shadingShader->SetUniform1i("m_Emissive", 10);

			RenderCommand::Disable(RendererEnum::FACE_CULL);

			Renderer::DrawQuad(Matrix4());
		}
	}

	void SceneRenderer::PostProcessPass(const Scene& scene)
	{
	}

	void SceneRenderer::DebugRendererPass(Scene& scene)
	{
		ZoneScoped;

		mShadingBuffer->Bind();
		{
			// Lines
			mLineMesh->Bind();

			Shader* shader = ShaderManager::GetShader("Resources/Shaders/debugLine.shader");
			shader->Bind();

			shader->SetUniformMat4f("u_Projection", mProjection);
			shader->SetUniformMat4f("u_View", mView);

			bool depthTestState = true;
			for (auto& l : mDebugLines)
			{
				shader->SetUniformVec4("u_Color", l.LineColor);
				shader->SetUniformVec3("u_StartPos", l.Start);
				shader->SetUniformVec3("u_EndPos", l.End);

				if (l.DepthTest)
				{

				}

				glLineWidth(l.Width);
				RenderCommand::DrawLines(0, 2);
			}

			shader->Unbind();

			shader = Nuake::ShaderManager::GetShader("Resources/Shaders/line.shader");
			shader->Bind();
			shader->SetUniform1f("u_Opacity", 0.5f);
			shader->SetUniformMat4f("u_Projection", mProjection);

			for (auto& shape : mDebugShapes)
			{
				if(shape.DepthTest)
				{
					glEnable(GL_DEPTH_TEST);
				}
				else
				{
					RenderCommand::Disable(RendererEnum::DEPTH_TEST);
				}

				shader->SetUniformVec4("u_Color", shape.LineColor);

				glLineWidth(shape.Width);
				Matrix4 view = mView;
				Physics::RigidbodyShapes shapeType = shape.Shape->GetType();
				switch (shapeType)
				{
					case Physics::RigidbodyShapes::BOX:
					{
						const Quat& globalRotation = glm::normalize(shape.Rotation);
						const Matrix4& rotationMatrix = glm::mat4_cast(globalRotation);

						view = glm::translate(view, shape.Position) * rotationMatrix;
						view = glm::scale(view, reinterpret_cast<Physics::Box*>(shape.Shape.get())->GetSize());
						
						shader->SetUniformMat4f("u_View", view);
						

						mBoxGizmo->Bind();
						RenderCommand::DrawLines(0, 26);
						break;
					}
					case Physics::RigidbodyShapes::SPHERE:
					{
						const Quat& globalRotation = glm::normalize(shape.Rotation);
						const Matrix4& rotationMatrix = glm::mat4_cast(globalRotation);

						view = glm::translate(view, shape.Position) * rotationMatrix;
						view = glm::scale(view, Vector3(reinterpret_cast<Physics::Sphere*>(shape.Shape.get())->GetRadius()));
						shader->SetUniformMat4f("u_View", view);

						mSphereGizmo->Bind();
						RenderCommand::DrawLines(0, 128);
						break;
					}
					case Physics::RigidbodyShapes::CAPSULE:
					{
						const Quat& globalRotation = glm::normalize(shape.Rotation);
						const Matrix4& rotationMatrix = glm::mat4_cast(globalRotation);

						view = glm::translate(view, shape.Position) * rotationMatrix;

						shader->SetUniformMat4f("u_View", view);

						const Physics::Capsule* capsule = reinterpret_cast<Physics::Capsule*>(shape.Shape.get());
						mCapsuleGizmo->UpdateShape(capsule->GetRadius(), capsule->GetHeight());
						mCapsuleGizmo->Bind();
						Nuake::RenderCommand::DrawLines(0, 264);
						break;
					}
					case Physics::RigidbodyShapes::CYLINDER:
					{
						const Quat& globalRotation = glm::normalize(shape.Rotation);
						const Matrix4& rotationMatrix = glm::mat4_cast(globalRotation);

						view = glm::translate(view, shape.Position) * rotationMatrix;

						const Physics::Cylinder* cylinder = reinterpret_cast<Physics::Cylinder*>(shape.Shape.get());

						shader->SetUniformMat4f("u_View", view);

						mCylinderGizmo->Bind();
						mCylinderGizmo->UpdateShape(cylinder->GetRadius(), cylinder->GetHeight());
						Nuake::RenderCommand::DrawLines(0, 264);
						break;
					}
				}
			}

			
		}
		mShadingBuffer->Unbind();
	}

	void SceneRenderer::SetSkeletonBoneTransformRecursive(Scene& scene, SkeletonNode& skeletonNode, Shader* shader)
	{
		for (auto& child : skeletonNode.Children)
		{
			if (auto entity = scene.GetEntity(child.Name); entity.GetHandle() != -1)
			{
				const std::string boneMatrixUniformName = "u_FinalBonesMatrice[" + std::to_string(child.Id) + "]";
				shader->SetUniformMat4f(boneMatrixUniformName, child.FinalTransform);
			}

			SetSkeletonBoneTransformRecursive(scene, child, shader);
		}
	}

}