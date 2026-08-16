#!/usr/bin/env ruby
# frozen_string_literal: true

require "rexml/document"
require "rexml/formatters/pretty"

project_root = File.expand_path("..", __dir__)
games_xml_path = File.expand_path(ARGV.fetch(0, File.join(project_root, "Config", "Games.xml")))
dat_path = File.expand_path(
  ARGV.fetch(1, File.join(project_root, "Config", "Supermodel (ClrMame Pro XML).dat"))
)

source = REXML::Document.new(File.read(games_xml_path))
source_games = {}
source.elements.each("games/game") do |game|
  source_games[game.attributes.fetch("name").to_s] = game
end

# Games.xml clone definitions contain only the ROM positions that differ from
# their parent. Expand that inheritance so the DAT describes the complete
# logical ROM set while marking parent ROMs as merge candidates.
effective_roms = {}
resolve_roms = lambda do |name|
  return effective_roms.fetch(name) if effective_roms.key?(name)

  game = source_games.fetch(name)
  parent = game.attributes["parent"]&.to_s
  roms = parent ? resolve_roms.call(parent).dup : {}

  game.elements.each("roms/region") do |region|
    region_name = region.attributes.fetch("name").to_s
    region.elements.each("file") do |file|
      key = [region_name, file.attributes.fetch("offset").to_s]
      roms[key] = {
        "name" => file.attributes.fetch("name").to_s,
        "crc" => file.attributes.fetch("crc32").to_s.sub(/\A0x/i, "").downcase
      }
    end
  end

  effective_roms[name] = roms.freeze
end

source_games.each_key { |name| resolve_roms.call(name) }

datafile = REXML::Element.new("datafile")
header = datafile.add_element("header")
header.add_element("name").text = "Supermodel - Sega Model 3 Games"
header.add_element("description").text = "Supermodel v0.3a-libretro Sega Model 3 Games"
header.add_element("category").text = "Standard DatFile"
header.add_element("version").text = "0.3a-libretro"
header.add_element("author").text = "The Supermodel Team"
header.add_element("homepage").text = "https://github.com/trzy/Supermodel"
header.add_element("url").text = "https://github.com/trzy/Supermodel"
header.add_element("clrmamepro").add_attribute("forcenodump", "ignore")

source_games.each_value do |source_game|
  name = source_game.attributes.fetch("name").to_s
  parent = source_game.attributes["parent"]&.to_s
  game = datafile.add_element("game", { "name" => name, "sourcefile" => "Games.xml" })
  if parent
    game.add_attribute("cloneof", parent)
    game.add_attribute("romof", parent)
  end

  title = source_game.elements["identity/title"]&.text.to_s.strip
  version = source_game.elements["identity/version"]&.text.to_s.strip
  description = version.empty? ? title : "#{title} (#{version})"
  game.add_element("description").text = description
  game.add_element("year").text = source_game.elements["identity/year"]&.text.to_s.strip
  game.add_element("manufacturer").text =
    source_game.elements["identity/manufacturer"]&.text.to_s.strip

  parent_roms = parent ? effective_roms.fetch(parent) : {}
  effective_roms.fetch(name).each do |key, rom|
    attributes = { "name" => rom.fetch("name"), "crc" => rom.fetch("crc") }
    parent_rom = parent_roms[key]
    if parent_rom == rom
      attributes["merge"] = rom.fetch("name")
    end
    game.add_element("rom", attributes)
  end
end

formatter = REXML::Formatters::Pretty.new(2)
formatter.compact = true

File.open(dat_path, "w") do |file|
  file.puts('<?xml version="1.0" encoding="UTF-8"?>')
  file.puts('<!DOCTYPE datafile PUBLIC "-//Logiqx//DTD ROM Management Datafile//EN" "http://www.logiqx.com/Dats/datafile.dtd">')
  file.puts
  formatter.write(datafile, file)
  file.puts
end

warn "Generated #{dat_path} (#{source_games.size} games)"
