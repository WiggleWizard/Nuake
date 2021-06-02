#pragma once
#include <src/Core/Timestep.h>
#include <src/Rendering/Framebuffer.h>
#include <src/UI/Rect.h>
#include "yoga/Yoga.h"
#include "Node.h"
#include <src/UI/Canvas.h>
#include "../Core/Maths.h"
#include "Stylesheet.h"
namespace UI
{
	class UserInterface
	{
	private:
		Ref<FrameBuffer> m_Framebuffer; // Texture of the interface.
		std::string m_Name;
		Ref<Canvas> Root;
		Ref<StyleSheet> m_Stylesheet;
		YGConfigRef yoga_config;
		YGNodeRef yoga_root;
	public:
		const int Width = 1920;
		const int Height = 1080;

		UserInterface(const std::string& name);
		~UserInterface();

		void Reload();

		static Ref<UserInterface> New(const std::string& name);
		void Calculate(int available_width, int available_height);

		void CreateYogaLayout();
		void CreateYogaLayoutRecursive(Ref<Node> node, YGNodeRef yoga_node);
		void Draw(Vector2 size);
		void DrawRecursive(Ref<Node> node, float z, Vector2 offset);
		void Update(Timestep ts);
	};
}