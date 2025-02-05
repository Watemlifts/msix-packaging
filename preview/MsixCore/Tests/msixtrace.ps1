param (
	[Parameter(ParameterSetName="start", Mandatory=$true)]
    [switch]$start,

    [Parameter(ParameterSetName="stop", Mandatory=$true)]
    [switch]$stop,

    [Parameter(ParameterSetName="wait", Mandatory=$true)]
    [switch]$wait,

	[Parameter(ParameterSetName="stop")]
    [Parameter(ParameterSetName="wait")]
    [string]$logFile
)

function DoStart
{
	Write-Host "Starting tracing..."
	$query = logman query MsixTrace
	if ($query.count -lt 4)
	{
		logman create trace MsixTrace -p "{033321d3-d599-48e0-868d-c59f15901637}" -o c:\msixtrace.etl
	}
	logman update MsixTrace -p "{db5b779e-2dcf-41bc-ab0e-40a6e02f1438}"

	logman start MsixTrace > $null
}

function LevelToString($level)
{
	switch ($level)
	{
	1{ return "Fatal"}
	2{ return "Error"}
	3{ return "Warning"}
	4{ return "Info"}
	5{ return "Verbose"}
	default { return "Level $level"}
	}
}

function DoStop
{
	Write-Host "Stopping tracing..."
	logman stop MsixTrace > $null

	#find the ETL file that was created
	$query = logman query MsixTrace
	$etlFile = ""
	foreach ($line in $query)
	{
		if ($line.endswith(".etl"))
		{
			$splits = $line.split()        
			$etlFile = $splits[$splits.count - 1]
		}
	}

	Write-Host "Parsing trace logs..."
	
	#convert the ETL file to XML data
	$now = [datetime]::Now.ToString("yyyy_MM_dd_HHmmss")
	$tempXmlfile = join-path $env:temp ("MsixTrace_{0}.xml" -f $now)
	tracerpt -l $etlFile -o $tempXmlFile > $null 

	if (-not $PSBoundParameters.ContainsKey('logFile')) 
	{
		#logfile parameter not specified, create a file in the current directory
		$logfile = join-path $pwd ("MsixTrace_{0}.log" -f $now)
		$warningfile = join-path $pwd ("MsixTraceWarnings_{0}.log" -f $now)
	}

	#convert XML data into readable text, filtering out "irrelevant" event data
	$xmlData = [xml] (get-content $tempXmlFile)

	# event 0 should be Header, start at 1
	for($i=1; $i -lt $xmldata.Events.Event.count; $i++)
	{
		$outputLine = $xmldata.Events.Event[$i].system.timecreated.systemtime + ", "
		$outputLine += LevelToString($xmldata.Events.Event[$i].system.level)
		$outputLine += ", " 
		$outputLine += $xmlData.Events.Event[$i].renderinginfo.task

		foreach ($data in $xmldata.Events.Event[$i].eventdata.data)
		{
			if (-not ($data -eq $null)) # on win7 this is necessary for some reason you can foreach on an null element and get an null object
			{
				if ($data.name.tostring().toupper() -eq "HR")
				{
					#convert int to hex for HResults (hardcoded fieldname)
					$outputLine += ", " + $data.name +": " + ("0x{0:x}" -f [System.Convert]::ToInt32($data.'#text'))
				}
				else
				{
					$outputLine += ", " + $data.name 
					if (($data.'#text'))
					{
						$outputLine += ": " + ($data.'#text').trim()
					}
					else
					{
						$outputLine += ": <null>"
					}
				}
			}
		}
			
		$outputLine >> $logfile
		if (-not $PSBoundParameters.ContainsKey('logFile') -and $xmldata.Events.Event[$i].system.level -lt 4) 
		{
			$hasWarnings = $true
			$outputLine >> $warningfile
		}
	}

	write-host "Raw logfile at " $etlFile
	write-host "Parsed logfile at " $logfile
	if (-not $PSBoundParameters.ContainsKey('logFile') -and $hasWarnings) 
	{
		write-host "Logfile contains warnings at " $warningfile
	}
}

if ($start)
{
    DoStart
}
elseif ($stop)
{
    DoStop
}
elseif ($wait)
{
    DoStart
    Read-Host "Tracing has started, press enter to stop"
    DoStop
}
