#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelAudioIn2);
	p->addModel(modelAudioIn8);
	p->addModel(modelAudioIn16);
	p->addModel(modelAudioOut2);
	p->addModel(modelAudioOut8);
	p->addModel(modelAudioOut16);
}
