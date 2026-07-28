# frozen_string_literal: true

require_relative "lib/helloworld_ruby/version"

Gem::Specification.new do |spec|
  spec.name = "helloworld-ruby"
  spec.version = HelloworldRuby::VERSION
  spec.authors = ["E2E"]
  spec.email = ["e2e@example.com"]
  spec.summary = "omnipackage e2e ruby hello-world"
  spec.files = Dir["lib/**/*", "exe/*"]
  spec.executables = ["helloworld-ruby"]
  spec.bindir = "exe"
  spec.required_ruby_version = ">= 2.7"
end
