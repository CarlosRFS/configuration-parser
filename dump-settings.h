#pragma once

#include <string>

class MetaConfiguration;

/* Generates the header file for the configuration */
void dump_header(const MetaConfiguration &config, const std::string &filename);

/* generates the source fole for the configuration */
void dump_source(const MetaConfiguration &config, const std::string &filename);
