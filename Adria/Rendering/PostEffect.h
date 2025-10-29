#pragma once
#include "RenderGraph/RenderGraphResourceName.h"

namespace adria
{
	class RenderGraph;
	class PostProcessor;

	class PostEffect
	{
	public:
		virtual ~PostEffect() = default;

		virtual void AddPass(RenderGraph&, PostProcessor*) = 0;
		virtual void OnResize(Uint32, Uint32) = 0;
		virtual Bool IsEnabled(PostProcessor const*) const = 0;
		virtual void OnSceneInitialized() {}
		virtual void GUI() {}
		virtual Bool IsGUIVisible(PostProcessor const* postprocessor) const { return true; }
		virtual Bool IsSupported() const { return true; }
	};

	class EmptyPostEffect : public PostEffect
	{
	public:
		EmptyPostEffect() {}

		virtual void AddPass(RenderGraph&, PostProcessor*) override {}
		virtual void OnResize(Uint32, Uint32) override {}
		virtual Bool IsEnabled(PostProcessor const*) const override { return false; }
		virtual Bool IsSupported() const override { return false; }
	};

	template<typename PostEffectT> requires std::is_base_of_v<PostEffect, PostEffectT>
	class TPostEffectGroup : public PostEffect
	{
	public:
		virtual void AddPass(RenderGraph& rg, PostProcessor* postprocessor) override
		{
			ADRIA_ASSERT(post_effect_idx < post_effects.size());
			if (post_effects[post_effect_idx]->IsEnabled(postprocessor))
			{
				post_effects[post_effect_idx]->AddPass(rg, postprocessor);
			}
		}
		virtual void OnResize(Uint32 w, Uint32 h) override
		{
			for (auto& post_effect : post_effects)
			{
				post_effect->OnResize(w, h);
			}
		}
		virtual Bool IsEnabled(PostProcessor const* postprocessor) const override
		{
			return post_effects[post_effect_idx]->IsEnabled(postprocessor);
		}
		virtual void OnSceneInitialized() override
		{
			for (auto& post_effect : post_effects)
			{
				post_effect->OnSceneInitialized();
			}
		}
		virtual void GUI() override
		{
			GroupGUI();
			post_effects[post_effect_idx]->GUI();
		}

	protected:
		std::vector<std::unique_ptr<PostEffectT>> post_effects;
		Uint32 post_effect_idx;

	protected:
		virtual void GroupGUI() {}
	};

	class PostEffectGroup : public TPostEffectGroup<PostEffect> {};
}