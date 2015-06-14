#!/usr/bin/env ruby

#
# This is a specialization of `newMachine.rb` to B.A.D.
#

require './lib/ec2'
require './lib/setup'
require './lib/deploy'
require 'optparse'

SSH_USER = 'ubuntu'
SSH_PKEY = {} # default
TAR_FILE = 'bad.tar.gz'

nameArg = nil
options = {}

def parseName(opts, name)
  names = name.split(',')
  if names.length > 1
    if names.length != opts[:count]
      $stderr.puts "Invalid number of names! (formant '<name1>,<name2>,...')"
      exit 0
    end
  else
    names = []
    for i in 1..opts[:count] do
      names += [ sprintf(name, i) ]
    end
  end
  opts[:name] = names
end

optparse = OptionParser.new do |opts|
  opts.banner = "Usage: #{$0} [options]\nCreate a set of new i2.xlarge instances for B.A.D."

  options[:zone] = "ap-southeast-1a"
  options[:group] = "default"
  options[:instance_type] = "i2.xlarge"
  options[:arch] = 0 # 64-bit
  options[:store] = 0
  options[:terminate] = 0

  opts.on("-k", "--key KEY_NAME", "Security key name") do |key|
    options[:key_name] = key
  end

  options[:count] = 1
  opts.on("-c", "--count NUMBER", "Number of instances to launch") do |c|
    options[:count] = c.to_i
    # we do this to make order of `--count` and `--name` irrelevant.
    if !nameArg.nil?
      parseName(options, nameArg)
    end
  end

  opts.on("-n", "--name NAME_PATTERN", "Name of the new instance") do |name|
    nameArg = name
    parseName(options, name)
  end

  options[:placement_group] = nil
  opts.on("-p", "--placement PLACEMENT_GROUP", "Placement group to launch in") do |pg|
    options[:placement_group] = pg
  end
end

if ARGV.length == 0
  optparse.parse %w[--help]
end

optparse.parse!

if options[:key_name].nil?
  puts "Must set a public key (`-k`) to use!"
  exit 1
end

# make '.' output for wait appear steady
$stdout.sync = true

i = 0
# launch instance
Launcher.new(options).launch! do |instance|
  i += 1
  puts "New instance (#{i}) at: #{instance.dns_name}"

  # wait for instance to be ready and SSH up
  puts "Waiting for instances to become ready..."
  timeout = 120
  while instance.status != :running && timeout > 0
    print '.'
    timeout = timeout - 1
    sleep 1
  end
  if timeout <= 0
    puts "Timeout waiting on instance to be ready!"
    exit 1
  end
  puts "Waiting on SSH to become ready..."
  sleep 10
  Net::SSH.wait(instance.dns_name, SSH_USER, SSH_PKEY)

  # setup instance
  puts "Setting up instance (#{i})..."
  conf = Setup.new(node: instance.dns_name, user: SSH_USER, skey: SSH_PKEY)
  conf.setup!
  puts "Instance (#{i}) setup for B.A.D!"

  # deploy bad
  puts "Deploying B.A.D distribution..."
  deployer = Deploy.new(hostname: instance.dns_name, user: SSH_USER,
                        skey: SSH_PKEY, distfile: TAR_FILE)
  deployer.deploy!
  puts "B.A.D deployed and ready!"
end
